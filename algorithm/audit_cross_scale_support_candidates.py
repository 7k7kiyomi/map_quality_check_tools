"""Read-only investigation of reported 502 findings against source SHP candidates.

Requires the LTZK OSGeo Python environment. Does not run the checker, change any
finding, or write anything into the user's result/source directories.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import unicodedata

from osgeo import gdal, ogr, osr


def normalize(text):
    return re.sub(r"\s+", "", unicodedata.normalize("NFKC", str(text or "")).strip().casefold())


def text_of(feature):
    fields = {feature.GetFieldDefnRef(i).GetName().casefold(): i for i in range(feature.GetFieldCount())}
    for name in ("text_conte", "text_content", "text", "name", "名称"):
        if name in fields:
            text = feature.GetFieldAsString(fields[name])
            if normalize(text):
                return normalize(text)
    return ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    gdal.UseExceptions()
    item = json.loads(Path(args.input).read_text(encoding="utf-8-sig"))["check_item1"]
    records = list(item.get("review_errors", {}).values())
    executions = {(x["pair_name"], x["target_layer"]): x for x in item["info"]["execution_records"]}
    cache = {}

    def candidates(path, crs):
        key = (path, crs)
        if key in cache:
            return cache[key]
        ds = ogr.Open(path, 0)
        if ds is None:
            raise RuntimeError("Cannot read source: " + path)
        layer = ds.GetLayer(0)
        src = layer.GetSpatialRef().Clone()
        dst = osr.SpatialReference()
        dst.ImportFromWkt(crs)
        src.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        dst.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        transform = None if src.IsSame(dst) else osr.CoordinateTransformation(src, dst)
        values = []
        for feature in layer:
            text = text_of(feature)
            original = feature.GetGeometryRef()
            if not text or original is None or original.IsEmpty():
                continue
            geom = original.Clone()
            if transform is not None:
                geom.Transform(transform)
            values.append((text, geom, path, feature.GetFID()))
        cache[key] = values
        return values

    bins = collections.Counter()
    outcomes = collections.Counter()
    samples = []
    per_layer = collections.defaultdict(collections.Counter)
    suffix_examples = []
    labels = [r for r in records if r.get("error_type", "").startswith("cross_scale_annotation_")]
    scopes = {}
    for record in labels:
        execution = executions[(record["pair_name"], record["layer"])]
        crs = execution["check_crs"]
        scope_wkt = execution["checked_scope_wkt"]
        if scope_wkt not in scopes:
            scopes[scope_wkt] = ogr.CreateGeometryFromWkt(scope_wkt)
        scope = scopes[scope_wkt]
        small = ogr.CreateGeometryFromWkt(record["checked_geometry_wkt"]).Centroid()
        text = record["parsed_text_identity"]
        available = [v for path in execution["large_paths"] for v in candidates(path, crs)]
        same = [v for v in available if v[0] == text]
        distances, full_distances = [], []
        for _, geom, path, fid in same:
            full_distances.append((small.Distance(geom.Centroid()), path, fid))
            if not geom.Intersects(scope):
                continue
            if "anchor" in record.get("small_logical_model", ""):
                xmin, xmax, ymin, ymax = geom.GetEnvelope()
                anchor = ogr.Geometry(ogr.wkbPoint)
                anchor.AddPoint_2D((xmin+xmax)/2, (ymin+ymax)/2)
            else:
                clipped = geom.Intersection(scope)
                if clipped is None or clipped.IsEmpty():
                    continue
                anchor = clipped.Centroid()
            distances.append((small.Distance(anchor), path, fid))
        nearest = min(distances) if distances else None
        tolerance = record.get("annotation_tolerance", 150)
        outcome = ("same_text_in_common_scope" if nearest else
                   "same_text_only_outside_common_scope" if same else "no_exact_text_in_confirmed_target_sources")
        outcomes[outcome] += 1
        per_layer[record["layer"]][outcome] += 1
        if nearest:
            distance = nearest[0]
            bucket = next((name for maximum, name in ((150, "0-150m"), (300, "150-300m"),
                (600, "300-600m"), (1500, "600-1500m"), (5000, "1500-5000m")) if distance <= maximum), ">5000m")
            bins[bucket] += 1
            if len(samples) < 18 or record["layer"] == "乡镇级驻地标注":
                samples.append({"text": text, "layer": record["layer"], "type": record["error_type"],
                    "nearest_same_text_distance_m": round(distance, 3), "tolerance_m": tolerance,
                    "small_path": record["right_shp_path"], "small_fid": record["right_fid"],
                    "large_path": nearest[1], "large_fid": nearest[2], "pair": record["pair_name"]})
        elif not same:
            base = re.sub(r"(?:村委会|行政村|自然村|[乡镇村县区市])$", "", text)
            possible = [v for v in available if len(base) >= 2
                and re.sub(r"(?:村委会|行政村|自然村|[乡镇村县区市])$", "", v[0]) == base]
            if possible and len(suffix_examples) < 20:
                suffix_examples.append({"small_text": text, "possible_large_texts": sorted({v[0] for v in possible}),
                    "layer": record["layer"], "note": "diagnostic only, not an accepted identity match"})
    nonlabels = [r for r in records if r not in labels]
    support_cache = {}

    def support_geometries(execution, anchor_mode):
        key = (tuple(execution["large_paths"]), execution["check_crs"], execution["checked_scope_wkt"], anchor_mode)
        if key in support_cache:
            return support_cache[key]
        scope = ogr.CreateGeometryFromWkt(execution["checked_scope_wkt"])
        dst = osr.SpatialReference()
        dst.ImportFromWkt(execution["check_crs"])
        dst.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        groups, values = {}, []
        for path in execution["large_paths"]:
            ds = ogr.Open(path, 0)
            layer = ds.GetLayer(0)
            src = layer.GetSpatialRef().Clone()
            src.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
            transform = None if src.IsSame(dst) else osr.CoordinateTransformation(src, dst)
            definition = layer.GetLayerDefn()
            sid_index, center_index = definition.GetFieldIndex("symbol_id"), definition.GetFieldIndex("symbol_cen")
            for feature in layer:
                original = feature.GetGeometryRef()
                if original is None or original.IsEmpty():
                    continue
                geom = original.Clone()
                if transform is not None:
                    geom.Transform(transform)
                if anchor_mode:
                    sid = feature.GetFieldAsString(sid_index) if sid_index >= 0 else ""
                    center = feature.GetFieldAsString(center_index) if center_index >= 0 else ""
                    instance = (path, sid, center) if sid and center else (path, feature.GetFID())
                    groups.setdefault(instance, []).append((geom, path, feature.GetFID()))
                elif geom.Intersects(scope):
                    clipped = geom.Intersection(scope)
                    if clipped is not None and not clipped.IsEmpty():
                        values.append((clipped, path, feature.GetFID()))
        for parts in groups.values():
            if not any(g.Intersects(scope) for g, _, _ in parts):
                continue
            envelopes = [g.GetEnvelope() for g, _, _ in parts]
            point = ogr.Geometry(ogr.wkbPoint)
            point.AddPoint_2D((min(e[0] for e in envelopes)+max(e[1] for e in envelopes))/2,
                (min(e[2] for e in envelopes)+max(e[3] for e in envelopes))/2)
            values.append((point, parts[0][1], parts[0][2]))
        support_cache[key] = values
        return values

    missing_distances, missing_examples = collections.Counter(), []
    for record in nonlabels:
        if record["error_type"] != "cross_scale_support_missing":
            continue
        execution = executions[(record["pair_name"], record["layer"])]
        small = ogr.CreateGeometryFromWkt(record["checked_geometry_wkt"])
        values = support_geometries(execution, "anchor" in record.get("small_logical_model", ""))
        nearest = min(((small.Distance(geom), path, fid) for geom, path, fid in values), default=None)
        if nearest is None:
            missing_distances["no_eligible_source_geometry_in_scope"] += 1
            continue
        bucket = next((name for maximum, name in ((150, "0-150m"), (300, "150-300m"),
            (600, "300-600m"), (1500, "600-1500m"), (5000, "1500-5000m")) if nearest[0] <= maximum), ">5000m")
        missing_distances[bucket] += 1
        if len(missing_examples) < 30:
            missing_examples.append({"layer": record["layer"], "small_path": record["right_shp_path"],
                "small_fid": record["right_fid"], "nearest_source_distance_m": round(nearest[0], 3),
                "nearest_source_path": nearest[1], "nearest_source_fid": nearest[2],
                "search_radius_m": record.get("candidate_search_radius"),
                "note": "spatial diagnosis only; eligibility still requires confirmed row, class and identity checks"})
    gates = collections.Counter()
    for record in nonlabels:
        value = record.get("support_failure_gates", [])
        if isinstance(value, list):
            gates.update(str(x) for x in value)
    report = {
        "input": args.input, "review_records": len(records), "annotation_reviews": len(labels),
        "type_counts": dict(collections.Counter(r["error_type"] for r in records)),
        "annotation_layers": {k: dict(v) for k, v in per_layer.items()},
        "annotation_source_outcomes": dict(outcomes), "nearest_eligible_same_text_distance_histogram": dict(bins),
        "annotation_samples": samples[:40], "suffix_difference_examples": suffix_examples,
        "nonannotation_failure_gates": dict(gates),
        "nonannotation_no_candidate_count": sum(r.get("candidate_count") == 0 for r in nonlabels),
        "nonannotation_outside_position_count": sum(r["error_type"] == "cross_scale_candidate_outside_position_tolerance" for r in nonlabels),
        "missing_support_nearest_source_distance_histogram": dict(missing_distances),
        "missing_support_examples": missing_examples,
        "limits": ["Read-only comparison within reported target-layer source lists; does not change confirmed mapping rows.",
            "Distances diagnose layout/selection limits; same text or a relaxed distance alone does not prove feature identity.",
            "A review finding is not a verified cartographic error."],
    }
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k not in ("annotation_samples", "suffix_difference_examples", "annotation_layers", "missing_support_examples")}, ensure_ascii=True))


if __name__ == "__main__":
    main()
