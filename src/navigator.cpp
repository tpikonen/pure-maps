#include "navigator.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGeoCoordinate>
#include <QLocale>
#include <QTime>

#include <s2/s2closest_edge_query.h>
#include <s2/s2earth.h>
#include <vector>
#include <cmath>

// NB! All distances in Rad unless having suffix _m for Meters

#define MAX_ROUTE_INTERSECTIONS 5  // Maximal number of route intersections with itself
#define REF_POINT_ADD_MARGIN    2  // Add next reference point along the route when it is that far away from the last one (relative to accuracy)
#define NUMBER_OF_REF_POINTS    2  // how many points to keep as a reference
#define MAX_OFFROAD_COUNTS      3  // times position was updated and point was off the route (counted in a sequence)
#define MAX_OFFROAD_COUNTS_TO_REROUTE 5 // request reroute when driving along route but in opposite direction. should be larger than MAX_OFFROAD_COUNTS
#define REROUTE_REQUEST_TIME_INTERVAL_MS 5000 // min time elapsed since last rerouting request in milliseconds

// use var without m_ prefix
#define SET(var, value) { auto t=(value); if (m_##var != t) { m_##var=t; qDebug() << "Emit " #var; emit var##Changed(); } }

Navigator::Navigator(QObject *parent) : QObject(parent)
{
  setupTranslator();
  connect(this, &Navigator::languageChanged, this, &Navigator::setupTranslator, Qt::DirectConnection);
}

void Navigator::setupTranslator()
{
  QString lang = m_language;
  if (lang == QLatin1Literal("en-US-x-pirate")) lang = QLatin1String("en-US");
  m_locale = QLocale(lang);
  if (m_translator.load(m_locale, APP_NAME, QLatin1String("-"),
                        QStringLiteral(DEFAULT_DATA_PREFIX "translations")))
    qDebug() << "Loaded translation for navigation" << m_translator.language();
  else
    qWarning() << "Translation not found for navigator:" << lang;
}

void Navigator::clearRoute()
{
  setRunning(false);
  m_polyline.release();
  m_route.clear();
  emit routeChanged();

  m_distance_traveled_m = 0;
  emit progressChanged();

  SET(totalDist, "");
  SET(totalTime, "");
  SET(destDist, "");
  SET(destEta, "");
  SET(destTime, "");
  SET(onRoute, false);
}

void Navigator::resetPrompts()
{
  m_last_prompt = 0;
  for (auto &p: m_prompts)
    {
      p.flagged = false;
      p.requested = false;
    }
  qDebug() << "Prompts reset";
}

double Navigator::progress() const
{
  if (!m_running)
    return m_last_distance_along_route_m  / std::max(1.0, m_route_length_m);
  return m_distance_traveled_m / std::max(1.0, m_distance_traveled_m + m_route_length_m - m_last_distance_along_route_m);
}

void Navigator::setPosition(const QGeoCoordinate &c, double horizontalAccuracy, bool valid)
{
  if (!m_index || !valid || horizontalAccuracy > 100) return;

  double accuracy_rad = S2Earth::MetersToRadians(horizontalAccuracy);
  S1ChordAngle accuracy = S1ChordAngle::Radians(accuracy_rad);
  S2Point point = S2LatLng::FromDegrees(c.latitude(), c.longitude()).ToPoint();

  // check if standing still
  if (m_last_point_initialized && m_last_point.Angle(point) < accuracy_rad/10)
    return;

  qDebug() << "Enter setPosition";

  // update travelled distance and last point
  if (m_last_point_initialized && m_running)
    m_distance_traveled_m += S2Earth::RadiansToMeters( m_last_point.Angle(point) );

  m_last_point_initialized = true;
  m_last_point = point;

  // find if are on the route
  S2ClosestEdgeQuery::PointTarget target(point);
  S2ClosestEdgeQuery query(m_index.get());
  query.mutable_options()->set_max_results(MAX_ROUTE_INTERSECTIONS);
  query.mutable_options()->set_max_distance(accuracy);

  PointInfo best;
  PointInfo ref;
  if (m_points.size() > 0) ref = m_points.front();

  for (const auto& result : query.FindClosestEdges(&target))
    {
      S2Point pr = query.Project(point, result);
      EdgeInfo &einfo = m_edges[result.edge_id()];

      // is the projected point between edge vertices?
      S2Shape::Edge edge = query.GetEdge(result);
      double dist_edge_v0 = S1ChordAngle(edge.v0, pr).radians();
      double dist_edge_v1 = S1ChordAngle(edge.v1, pr).radians();
      double dist_edge = einfo.length;

      // replace point with vertex if it is projected outside the edge
      bool betweenVertexes = true;
      if ( dist_edge < dist_edge_v0 )
        {
          pr = edge.v1;
          dist_edge_v0 = dist_edge;
          betweenVertexes = false;
        }
      else if ( dist_edge < dist_edge_v1 )
        {
          pr = edge.v0;
          dist_edge_v0 = 0;
          betweenVertexes = false;
        }

      double dist = einfo.length_before + dist_edge_v0;
      if (!best || (ref &&
                    ref.length_on_route - accuracy_rad < dist && // is it along route?
                    // consider to improve only if on the edge. otherwise
                    // turns will always prefer older edge
                    betweenVertexes &&
                    // take the point closer to the origin only if
                    // it is significantly closer
                    (best.length_on_route - dist) > 2*REF_POINT_ADD_MARGIN*accuracy_rad))
        {
          // update to the current estimate
          best.length_on_route = dist;
          best.point = pr;
          best.bearing = einfo.bearing;
          best.maneuver = einfo.maneuver;
        }
    }

  // whether we found the point on route and whether it was in expected direction
  bool on_route = ((bool)best && (!ref || ref.length_on_route - accuracy_rad < best.length_on_route));

  if (on_route)
    {
      best.accuracy = accuracy_rad;
      m_last_distance_along_route_m = S2Earth::RadiansToMeters(best.length_on_route);
      m_distance_to_route_m = 0;

      const Maneuver &man = m_maneuvers[best.maneuver];
      m_last_duration_along_route = man.duration_on_route;
      if (man.length > 0)
        m_last_duration_along_route +=
            (best.length_on_route - man.length_on_route) / man.length * man.duration;

      SET(destDist, distanceToStr(m_route_length_m - m_last_distance_along_route_m));
      SET(destTime, timeToStr(m_route_duration - m_last_duration_along_route));

      QTime time = QTime::currentTime().addSecs(m_route_duration - m_last_duration_along_route);
      SET(destEta, QLocale::system().toString(time, QLocale::ShortFormat));

      // handle reference points
      if (!ref || // add the first reference point
          (best.length_on_route - m_points.back().length_on_route) / best.accuracy > REF_POINT_ADD_MARGIN)
        m_points.push_back(best);

      if ( m_points.size() > NUMBER_OF_REF_POINTS ||
           (m_points.size() > 0 && m_points.front().accuracy/2 > best.accuracy) )
        m_points.pop_front();

      emit progressChanged();

      // handle onRoute specially as some lockups were seen when setting
      // it early in the method
      on_route = (m_points.size() >= NUMBER_OF_REF_POINTS);
      if (on_route)
        {
          SET(bearing, best.bearing);
          m_offroad_count = 0;
        }

      // reset prompts when just entering the route
      if (on_route && !m_onRoute)
        resetPrompts();

      if (on_route && best.maneuver+1 < m_maneuvers.size())
        {
          const Maneuver &next = m_maneuvers[best.maneuver+1];
          const double mdist = S2Earth::RadiansToMeters(next.length_on_route - best.length_on_route);
          const double mtime = next.duration_on_route - m_last_duration_along_route;
          SET(manDist, distanceToStr(mdist));
          SET(manTime, timeToStr(mtime));
          SET(icon, next.icon);
          SET(narrative, next.narrative);
          if (m_running && (mdist < 500 || mtime < 300)) { SET(sign, next.sign); }
          else SET(sign, QVariantMap());
          SET(street, next.street);

          // check for voice prompt to play
          for (size_t i=m_last_prompt; m_running && i < m_prompts.size(); ++i)
            {
              Prompt &p = m_prompts[i];
              if (!p.flagged &&
                  (p.dist_m <= m_last_distance_along_route_m || p.time <= m_last_duration_along_route))
                {
                  if (p.dist_m+p.length() < m_last_distance_along_route_m &&
                      p.time+p.duration() < m_last_duration_along_route)
                    {
                      qDebug() << "Skipping prompt as it is too late:"
                               << p.dist_m << m_last_distance_along_route_m << p.time
                               << p.text;
                    }
                  else
                    {
                      emit promptPrepare(p.text, false);
                      emit promptPlay(p.text);
                    }

                  p.flagged = true;
                  m_last_prompt = i;
                }
            }

          // prepare prompts for near future
          for (size_t i=m_last_prompt+1;
               m_running && i < m_prompts.size() &&
               m_prompts[i].time < m_last_duration_along_route + 300; ++i)
            {
              Prompt &p = m_prompts[i];
              if (!p.requested)
                {
                  p.requested = true;
                  emit promptPrepare(p.text, false);
                }
            }

        }
      else if (!on_route)
        {
          SET(manDist, "-");
          SET(narrative, trans("Preparing to start navigation"));
        }

      SET(onRoute, on_route);
    }
  else
    {
      if (m_offroad_count <= MAX_OFFROAD_COUNTS_TO_REROUTE) m_offroad_count++;

      // wipe history used to track direction on route if we are off the route
      if (m_offroad_count > MAX_OFFROAD_COUNTS)
        {
          m_points.clear();

          if ((bool)best) // point on route but wrong direction
            {
              m_distance_to_route_m = 0; // within the accuracy

              SET(icon, QLatin1String("uturn")); // turn around
              SET(narrative, trans("Moving in a wrong direction"));
              SET(manDist, "-");
            }
          else
            {
              S2ClosestEdgeQuery query(m_index.get());
              query.mutable_options()->set_max_results(1);
              m_distance_to_route_m = S2Earth::RadiansToMeters(query.GetDistance(&target).radians());

              SET(icon, QLatin1String("flag")); // away from route icon
              SET(narrative, trans("Away from route"));
              SET(manDist, distanceToStr(m_distance_to_route_m));
            }

          SET(manTime, QLatin1String());
          SET(sign, QVariantMap());
          SET(street, QLatin1String());
          SET(onRoute, false);
        }

      if ( ((m_offroad_count > MAX_OFFROAD_COUNTS_TO_REROUTE && m_distance_to_route_m < 1) ||
            m_distance_to_route_m > 100 + horizontalAccuracy) &&
          m_reroute_request.elapsed() > REROUTE_REQUEST_TIME_INTERVAL_MS)
        {
          m_reroute_request.restart();
          emit rerouteRequest();
        }
    }

  qDebug() << "Exit setPosition";
}


void Navigator::setRoute(QVariantMap m)
{
  const double accuracy_m = 0.1;
  const double accuracy = S2Earth::MetersToRadians(accuracy_m);

  // copy route coordinates
  QVariantList x = m.value("x").toList();
  QVariantList y = m.value("y").toList();
  if (x.length() != y.length())
    {
      qWarning() << "Route given by inconsistent lists: " << x.length() << " " << y.length();
      clearRoute();
      return;
    }

  // cleanup
  m_route.clear();
  m_edges.clear();
  m_points.clear();
  m_maneuvers.clear();
  m_reroute_request.start();

  // clear distance traveled only if not running
  // that will keep progress intact on rerouting
  if (!m_running) m_distance_traveled_m = 0;

  // set global vars
  SET(language, m.value("language", "en").toString());
  SET(mode, m.value("mode", "car").toString());

  // route
  m_route.reserve(x.length());
  for (int i=0; i < x.length(); ++i)
    {
      QGeoCoordinate c(y[i].toDouble(), x[i].toDouble());
      // avoid the same point entered twice (observed with MapQuest)
      if (i == 0 || c.distanceTo(m_route.back()) > accuracy_m)
        m_route.append(c);
    }
  emit routeChanged();

  std::vector<S2LatLng> coor;
  coor.reserve(x.length());
  for (QGeoCoordinate c: m_route)
    coor.push_back(S2LatLng::FromDegrees(c.latitude(), c.longitude()));

  m_polyline.reset(new S2Polyline(coor));

  // fill index
  m_index.reset(new MutableS2ShapeIndex);

  // determine each edge length
  int shape_id = m_index->Add(std::unique_ptr<S2Shape>(new S2Polyline::Shape(m_polyline.get())));
  const S2Shape *shape = m_index->shape(shape_id);
  double route_length = 0.0;
  for (int i=0; i < shape->num_edges(); ++i)
    {
      EdgeInfo edge;
      edge.length = S1ChordAngle(shape->edge(i).v0, shape->edge(i).v1).radians();
      edge.length_before = route_length;
      edge.bearing = S2Earth::GetInitialBearing(S2LatLng(shape->edge(i).v0), S2LatLng(shape->edge(i).v1)).degrees();
      m_edges.push_back(edge);
      route_length += edge.length;
    }

  m_route_length_m = S2Earth::RadiansToMeters(route_length);

  // fill maneuvers and voice prompts
  QVariantList man = m.value("maneuvers").toList();
  int edge_ind = 0;
  double length_on_route = 0;
  double duration_on_route = 0;
  double pre_duration = 0;
  double pre_length = 0;
  double pre_speed = 0;
  std::vector<Prompt> prompts;
  for (int mind = 0; mind < man.length(); ++mind)
    {
      QVariantMap mc = man[mind].toMap();
      // skip passive maneuvers
      if (mc.value("passive", false).toBool()) continue;

      const size_t new_man_ind = m_maneuvers.size();
      S2Point end;
      bool end_available = false;
      if (mind < man.length()-1)
        {
          QVariantMap mn = man[mind+1].toMap();
          end = S2LatLng::FromDegrees( mn.value("y").toDouble(), mn.value("x").toDouble() ).ToPoint();
          end_available = true;
        }

      double man_length = 0.0;
      for (; edge_ind < shape->num_edges() &&
             (!end_available || shape->edge(edge_ind).v0.Angle(end) > accuracy);
           ++edge_ind)
        {
          EdgeInfo &edge = m_edges[edge_ind];
          man_length += edge.length;
          edge.maneuver = new_man_ind;
        }

      Maneuver m(mc);
      m.duration_on_route = duration_on_route;
      m.length = man_length;
      m.length_on_route = length_on_route;
      m_maneuvers.push_back(m);
      length_on_route += man_length;
      duration_on_route += m.duration;

//      qDebug() << mc;
//      qDebug() << "Maneuver" << new_man_ind << "Length" << S2Earth::RadiansToKm(m.length) << "km  Duration"
//               << m.duration << "s  Speed" << S2Earth::RadiansToKm(m.length)/std::max(m.duration, 0.1)*3600 << "km/h" << "\n"
//               << m.duration_on_route << "s / " << S2Earth::RadiansToMeters(m.length_on_route) << "m"
//               << m.icon << m.narrative << m.sign << m.street << "\n";

      // fill prompts
      double post_duration = std::max(1.0, m.duration);
      double post_length = S2Earth::RadiansToMeters(m.length);
      double post_speed = post_length / post_duration;

      // Add advance alert, e.g. "In 1 km, turn right onto Broadway."
      if (!m.verbal_alert.isEmpty() && pre_duration > 1800 && pre_length > 30)
        {
          Prompt p = makePrompt(m,
                                // TRANSLATORS: Example: In 500 m, turn right onto Broadway. Translate "In" and adjust the order if needed
                                trans("In %2, %1").arg(m.verbal_alert),
                                std::min(500.0, pre_length-30),
                                std::min(90.0, pre_duration-3),
                                pre_speed, 1);
          prompts.push_back(p);
        }

      // Add advance alert, e.g. "In 100 m, turn right onto Broadway."
      if (!m.verbal_alert.isEmpty() && pre_duration > 20)
        {
          Prompt p = makePrompt(m,
                                trans("In %2, %1").arg(m.verbal_alert),
                                std::min(100.0, pre_length-30),
                                std::min(30.0, pre_duration-3),
                                pre_speed, 4);
          prompts.push_back(p);
        }

      // Add pre-maneuver prompt, e.g. "Turn right onto Broadway."
      if (!m.verbal_pre.isEmpty() && pre_duration > 2)
        {
          Prompt p = makePrompt(m,
                                m.verbal_pre,
                                std::min(50.0, pre_length-10),
                                std::min(5.0, pre_duration-1),
                                pre_speed, 3);
          prompts.push_back(p);
        }

      // Add post-maneuver prompt, e.g. "Continue for 100 m."
      if (!m.verbal_post.isEmpty() && post_duration > 20)
        {
          Prompt p = makePrompt(m,
                                m.verbal_post,
                                std::min(50.0, post_length-30),
                                std::min(5.0, post_duration-3),
                                post_speed, 2, true);
          prompts.push_back(p);
        }

      pre_duration = post_duration;
      pre_length = post_length;
      pre_speed = post_speed;
    }

  // remove overlapping prompts
  for (size_t i=0; prompts.size() > 1 && i < prompts.size()-1; ++i)
    {
      if (prompts[i].flagged) continue;

      size_t j = i+1;
      bool done = false;
      Prompt &p0 = prompts[i];
      double dist_0 = p0.dist_m + p0.length();
      double time_0 = p0.time + p0.duration();
      while (!done)
        {
          for (; j < prompts.size() && prompts[j].flagged; ++j);
          if (j >= prompts.size()) break;
          Prompt &p1 = prompts[j];
          bool overlap = (dist_0 >= p1.dist_m || time_0 >= p1.time);
          if (overlap)
            {
              if (p1.importance > p0.importance)
                {
                  p0.flagged = true;
                  done = true;
                }
              else
                p1.flagged = true;
            }
          else
            done = true;
        }
    }

  for (auto p: prompts)
    qDebug() << p.dist_m << p.dist_m + p.length() << p.time << p.time+p.duration()
             << p.importance << p.flagged << p.text;

  m_prompts.clear();
  for (auto p: prompts)
    if (!p.flagged)
      m_prompts.push_back(p);

  m_route_duration = duration_on_route;

  SET(totalDist, distanceToStr(m_route_length_m));
  SET(totalTime, timeToStr(m_route_duration));
}


void Navigator::setUnits(QString u)
{
  m_units = u;
  emit unitsChanged();
}

void Navigator::setRunning(bool r)
{
  if (!m_index && r)
    {
      qCritical() << "Navigator: Cannot start routing without route. Fix the caller.";
      r = false;
    }
  m_running = r;
  emit runningChanged();
}

// Translations and string functions
QString Navigator::trans(const char *text) const
{
  QString t = m_translator.translate("", text);
  if (t.isEmpty()) return text;
  return t;
}

static double roundToDigits(double n, int roundDig)
{
  double rd = std::pow(10, roundDig);
  return round(n/rd) * rd;
}

QString Navigator::n2Str(double n, int roundDig) const
{
  return m_locale.toString( roundToDigits(n, roundDig) );
}

QString Navigator::distanceToStr_american(double feet, bool condence) const
{
  QString unit;
  if (feet > 1010)
    {
      unit = condence ? trans("mi") : trans("miles");
      return QStringLiteral("%1 %2").arg(n2Str(feet/5280, feet > 5280 ? 0 : -1)).arg(unit);
    }
  unit = condence ? trans("ft") : trans("feet");
  return QString("%1 %2").arg(n2Str(feet, feet > 150 ? 2 : 1)).arg(unit);
}

QString Navigator::distanceToStr_british(double yard, bool condence) const
{
  QString unit;
  if (yard > 1010)
    {
      unit = condence ? trans("mi") : trans("miles");
      return QString("%1 %2").arg(n2Str(yard/1760, yard > 1760 ? 0 : -1)).arg(unit);
    }
  unit = condence ? trans("yd") : trans("yards");
  return QString("%1 %2").arg(n2Str(yard, yard > 150 ? 2 : 1)).arg(unit);
}

QString Navigator::distanceToStr_metric(double meters, bool condence) const
{
  QString unit;
  if (meters > 1000)
    {
      unit = condence ? trans("km") : trans("kilometers");
      return QString("%1 %2").arg(n2Str(meters/1000, 0)).arg(unit);
    }
  unit = condence ? trans("m") : trans("meters");
  return QString("%1 %2").arg(n2Str(meters, meters > 150 ? 2 : 1)).arg(unit);
}

QString Navigator::distanceToStr(double meters, bool condence) const
{
  if (m_units == QLatin1String("american"))
    return distanceToStr_american(3.28084 * meters, condence);
  if (m_units == QLatin1String("british"))
    return distanceToStr_british(1.09361 * meters, condence);
  return distanceToStr_metric(meters, condence);
}

QString Navigator::timeToStr(double seconds) const
{
  int hours = int(seconds / 3600);
  int minutes = round((seconds - hours*3600)/60);
  // TRANSLATORS: Keep %1 and %2 as they are, will be replaced with numerical hours (%1) and minutes (%2)
  return hours > 0 ? trans("%1 h %2 min").arg(hours).arg(minutes) :
                     // TRANSLATORS: Keep %1 as it is, it will be replaced with numerical minutes (%1)
                     trans("%1 min").arg(minutes);
}

double Navigator::distanceRounded(double meters) const
{
  const double mile = 1609.34;
  const double yard = 0.9144;
  const double foot = 0.3048;

  if (m_units == QLatin1String("american"))
    {
      if (meters >= mile) return roundToDigits(meters/mile, 0) * mile;
      int n = std::min(1, (int)ceil(log10(meters/foot)));
      return roundToDigits(meters/foot, n) * foot;
    }
  if (m_units == QLatin1String("british"))
    {
      if (meters >= mile) return roundToDigits(meters/mile, 0) * mile;
      int n = std::min(1, (int)ceil(log10(meters/yard)));
      return roundToDigits(meters/yard, n) * yard;
    }

  if (meters >= 1000) return roundToDigits(meters/1000, 0) * 1000;
  int n = std::min(1, (int)ceil(log10(meters)));
  return roundToDigits(meters, n);
}

Prompt Navigator::makePrompt(const Maneuver &m, QString text, double dist_offset_m, double time_offset,
                             double speed_m, int importance, bool after) const
{
  double distance = std::max(dist_offset_m, time_offset*speed_m);
  distance = distanceRounded(distance);
  Prompt p;
  p.dist_m = S2Earth::RadiansToMeters(m.length_on_route) + (after ? 1 : -1)*distance;
  p.importance = importance;
  p.speed_m = speed_m;
  p.time = m.duration_on_route + (after ? 1 : -1)*time_offset;
  if (text.contains('%')) p.text = text.arg( distanceToStr(distance, false) );
  else p.text = text;
  return p;
}

void Navigator::prepareStandardPrompts()
{
  m_std_prompts.clear();
  m_std_prompts.insert(QLatin1String("std:starting navigation"), trans("Starting navigation"));
  m_std_prompts.insert(QLatin1String("std:new route found"), trans("New route found"));
  m_std_prompts.insert(QLatin1String("std:rerouting"), trans("Rerouting"));
  m_std_prompts.insert(QLatin1String("std:rerouting failed"), trans("Rerouting failed"));

  // first prompt that is needed, can request multiple times
  emit promptPrepare(m_std_prompts["std:starting navigation"], true);
  for (QString p: m_std_prompts.values())
    emit promptPrepare(p, true);
}

void Navigator::prompt(const QString key)
{
  QString p = m_std_prompts.value(key);
  if (!p.isEmpty())
    emit promptPlay(p);
}
