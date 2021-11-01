# -*- coding: utf-8 -*-

# Copyright (C) 2015 Osmo Salomaa, 2018 Rinigus
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

"""
Routing using Valhalla at Stadia Maps.

https://docs.stadiamaps.com/
"""

import copy
import json
import poor
import urllib.parse

CONF_DEFAULTS = {
    "bicycle_type": "Hybrid",
    "language": poor.util.get_default_language("en"),
    "max_hiking_difficulty": 1,
    "shorter": 0,
    "type": "auto",
    "use_bus": 0.5,
    "use_ferry": 0.5,
    "use_highways": 1.0,
    "use_hills": 0.5,
    "use_primary": 0.5,
    "use_rail": 0.5,
    "use_roads": 0.5,
    "use_tolls": 0.5,
    "use_trails": 0.0,
    "use_transfers": 0.5,
}

ICONS = {
     0: "flag",
     1: "depart",
     2: "depart-right",
     3: "depart-left",
     4: "arrive",
     5: "arrive-right",
     6: "arrive-left",
     7: "continue",
     8: "continue",
     9: "turn-slight-right",
    10: "turn-right",
    11: "turn-sharp-right",
    12: "uturn",
    13: "uturn",
    14: "turn-sharp-left",
    15: "turn-left",
    16: "turn-slight-left",
    17: "continue",
    18: "off-ramp-slight-right",
    19: "off-ramp-slight-left",
    20: "off-ramp-slight-right",
    21: "off-ramp-slight-left",
    22: "fork-straight",
    23: "fork-slight-right",
    24: "fork-slight-left",
    25: "merge-slight-left",
    26: "roundabout",
    27: "off-ramp-slight-right",
    28: "ferry",
    29: "depart",
    30: "flag",
    31: "flag",
    32: "flag",
    33: "flag",
    34: "flag",
    35: "flag",
    36: "flag",
}

MODE = {
    "auto": "car",
    "auto_shorter": "car",
    "bicycle": "bicycle",
    "bus": "car",
    "hov": "car",
    "motorcycle": "car",
    "motor_scooter": "car",
    "pedestrian": "foot",
    "transit": "transit"
}

MODEOPTIONS = {
    "auto": ["use_ferry", "use_highways", "use_tolls"],
    "auto_shorter": ["use_ferry", "use_highways", "use_tolls"],
    "bicycle": ["bicycle_type", "use_ferry", "use_hills", "use_roads"],
    "bus": ["use_ferry", "use_highways", "use_tolls"],
    "hov": ["use_ferry", "use_highways", "use_tolls"],
    "motorcycle": ["use_ferry", "use_highways", "use_tolls", "use_trails"],
    "motor_scooter": ["use_ferry", "use_highways", "use_hills", "use_primary", "use_tolls"],
    "pedestrian": ["use_ferry", "max_hiking_difficulty"],
    "transit": ["use_bus", "use_rail", "use_transfers"]
}

URL = "https://route.stadiamaps.com/route?api_key=" + poor.key.get("STADIAMAPS_KEY") + "&json={input}"
URL_OPT = "https://route.stadiamaps.com/optimized_route?api_key=" + poor.key.get("STADIAMAPS_KEY") + "&json={input}"
cache = {}

def prepare_endpoint(point):
    """Return `point` as a dictionary ready to be passed on to the router."""
    if isinstance(point, (list, tuple)):
        return dict(lat=point[1], lon=point[0])
    if isinstance(point, dict):
        d = dict(lat=point["y"], lon=point["x"])
        if "text" in point: d["name"] = point["text"]
        if "destination" in point and not point["destination"]:
            d["type"] = "break_through"
        else:
            d["type"] = "break"
        return d
    geocoder = poor.Geocoder("default")
    results = geocoder.geocode(point, params=dict(limit=1))
    return prepare_endpoint((results[0]["x"], results[0]["y"]))

def route(locations, params):
    """Find route and return its properties as a dictionary."""
    loc = list(map(prepare_endpoint, locations))
    heading = params.get('heading', None)
    optimized = params.get('optimized', False) if len(loc) > 3 else False
    if heading is not None:
        loc[0]["heading"] = heading
    language = poor.conf.routers.stadiamaps.language
    units = "kilometers" if poor.conf.units == "metric" else "miles"
    ctype = poor.conf.routers.stadiamaps.type
    if ctype == "auto" and poor.conf.routers.stadiamaps.shorter: ctype = "auto_shorter"
    co = {key: poor.conf.routers.stadiamaps[key] for key in MODEOPTIONS[ctype]}
    costing_options = {}
    costing_options[ctype] = co
    input = dict(locations=loc,
                 costing=ctype,
                 costing_options=costing_options,
                 directions_options=dict(language=language, units=units))

    input = urllib.parse.quote(json.dumps(input))
    if optimized:
        url = URL_OPT.format(**locals())
    else:
        url = URL.format(**locals())
    with poor.util.silent(KeyError):
        return copy.deepcopy(cache[url])
    result = poor.http.get_json(url)
    result = poor.AttrDict(result)
    mode = MODE.get(ctype,"car")
    return parse_result_valhalla(url, locations, optimized, result, mode)

def parse_exit(maneuver, key):
    if "sign" not in maneuver or key not in maneuver["sign"]:
        return None
    e = maneuver["sign"][key]
    return [i.get("text", "") for i in e]

def parse_result_valhalla(url, locations, optimized, result, mode):
    """Parse and return route from Valhalla engine."""
    X, Y, Man, LocPointInd = [], [], [], [0]
    for legs in result.trip.legs:
        x, y = poor.util.decode_epl(legs.shape, precision=6)
        maneuvers = [dict(
            x=float(x[maneuver.begin_shape_index]),
            y=float(y[maneuver.begin_shape_index]),
            icon=ICONS.get(maneuver.type, "flag"),
            narrative=maneuver.instruction,
            sign=dict(
                exit_branch=parse_exit(maneuver, "exit_branch_elements"),
                exit_name=parse_exit(maneuver, "exit_name_elements"),
                exit_number=parse_exit(maneuver, "exit_number_elements"),
                exit_toward=parse_exit(maneuver, "exit_toward_elements")
            ),
            street=maneuver.get("begin_street_names", maneuver.get("street_names", None)),
            arrive_instruction=maneuver.get("arrive_instruction", None),
            depart_instruction=maneuver.get("depart_instruction", None),
            roundabout_exit_count=maneuver.get("roundabout_exit_count", None),
            travel_type=maneuver.get("travel_type", None),
            verbal_alert=maneuver.get("verbal_transition_alert_instruction", None),
            verbal_pre=maneuver.get("verbal_pre_transition_instruction", None),
            verbal_post=maneuver.get("verbal_post_transition_instruction", None),
            duration=float(maneuver.time),
        ) for maneuver in legs.maneuvers]
        X.extend(x)
        Y.extend(y)
        Man.extend(maneuvers)
        LocPointInd.append(len(X)-1)
    route = dict(x=X, y=Y,
                 locations=[locations[i.original_index] for i in result.trip.locations],
                 location_indexes=LocPointInd,
                 maneuvers=Man, mode=mode, optimized=optimized)
    route["language"] = result.trip.language.replace('-','_')
    if route and route["x"]:
        cache[url] = copy.deepcopy(route)
    return route
