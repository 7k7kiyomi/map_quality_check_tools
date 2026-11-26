"""Read-only PDF/SHP paint provenance check used by series checks 501 and 502.

This checks palette/point-width compatibility, not geographic feature identity or
normative symbol compliance. It never modifies the PDF or the source shapefiles.
"""
import argparse
import collections
import hashlib
import json
import math
import os
import re
import sys
from pathlib import Path

local_packages=Path(__file__).resolve().parent / 'python_packages'
sys.path.insert(0, str(local_packages if local_packages.exists() else Path('E:/LTZK/MPQIS/preprocess_scripts/python_packages')))
runtime = Path(__file__).resolve().parents[2] / 'OSGEO4W_32815'
if not runtime.exists(): runtime = Path('E:/LTZK/OSGEO4W_32815')
os.environ['GDAL_DATA'] = str(runtime / 'share/gdal')
os.environ['PROJ_LIB'] = str(runtime / 'share/proj')
os.environ['PROJ_DATA'] = str(runtime / 'share/proj')
from osgeo import gdal, ogr
import fitz

VERSION = 5


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def layer_key(name):
    name = re.sub(r'_(?:MultiLineString|LineString|MultiPolygon|Polygon|MultiPoint|Point|线|面|点)$', '', name, flags=re.I)
    return re.sub(r'[\s_—\-（）()]+', '', name).casefold()


def rgb_variants(text):
    text = str(text).strip()
    if text.startswith('#') and len(text) == 7:
        try:
            return [tuple(int(text[i:i+2], 16) / 255 for i in (1, 3, 5))]
        except ValueError:
            return []
    numbers = [float(x) for x in re.findall(r'[-+]?\d+(?:\.\d+)?', text)]
    if text.lower().startswith('cmyk') and len(numbers) == 4 and all(0 <= x <= 100 for x in numbers):
        c, m, y, k = [x / 100 for x in numbers]
        simple = ((1-c)*(1-k), (1-m)*(1-k), (1-y)*(1-k))
        pixel = fitz.Pixmap(fitz.csCMYK, fitz.IRect(0, 0, 1, 1))
        pixel.set_pixel(0, 0, tuple(round(x * 255 / 100) for x in numbers))
        converted = fitz.Pixmap(fitz.csRGB, pixel)
        return [simple, tuple(x / 255 for x in converted.samples[:3])]
    if text.lower().startswith('rgb') and len(numbers) == 3:
        divisor = 1 if max(numbers) <= 1 else 255
        if all(0 <= x <= divisor for x in numbers):
            return [tuple(x/divisor for x in numbers)]
    return []


def drawn(value):
    return str(value).strip().casefold() in ('true', '1', 'yes')


def audit(pdf, directory, output):
    gdal.UseExceptions()
    ogr.UseExceptions()
    files = sorted(directory.glob('*.shp'))
    signatures = {}
    for shp in files:
        for suffix in ('.shp', '.shx', '.dbf', '.prj'):
            sidecar = shp.with_suffix(suffix)
            if sidecar.exists(): signatures[sidecar.name] = digest(sidecar)
    pdf_hash = digest(pdf)
    signature = {'version': VERSION, 'pdf_sha256': pdf_hash, 'shapefiles': signatures,
                 'pdf_path': str(pdf.resolve()), 'shp_dir': str(directory.resolve())}
    if output.exists():
        try:
            cached = json.loads(output.read_text(encoding='utf-8-sig'))
            if cached.get('signature') == signature:
                return cached
        except (ValueError, OSError):
            pass
    report = dict(signature=signature, pdf_path=str(pdf.resolve()), pdf_sha256=pdf_hash,
                  shp_dir=str(directory.resolve()), status='completed',
                  method='original_pdf_paint_palette_compatibility', layers={})
    palettes = collections.defaultdict(lambda: {'stroke_col': set(), 'fill_color': set(), 'widths': set()})
    document = fitz.open(str(pdf))
    for page in document:
        for paint in page.get_drawings():
            name = layer_key(paint.get('layer') or '')
            if not name: continue
            kind = paint.get('type', '')
            if 's' in kind and paint.get('color') is not None:
                palettes[name]['stroke_col'].add(tuple(paint['color']))
                width = paint.get('width')
                if width is not None and math.isfinite(width): palettes[name]['widths'].add(float(width))
            if 'f' in kind and paint.get('fill') is not None:
                palettes[name]['fill_color'].add(tuple(paint['fill']))
    for path in files:
        result = {'status': 'completed', 'colors': {}, 'widths_pt': {}, 'semantic_field_warnings': {}}
        report['layers'][path.name] = result
        palette = palettes.get(layer_key(path.stem))
        try:
            gdal.ErrorReset()
            source = ogr.Open(str(path)); layer = source.GetLayer()
            colors = {'stroke_col': collections.Counter(), 'fill_color': collections.Counter()}
            widths = set(); original_closed = collections.Counter()
            result['read_feature_count'] = 0
            result['driver'] = source.GetDriver().GetName()
            result['feature_count'] = layer.GetFeatureCount()
            for feature in layer:
                result['read_feature_count'] += 1
                attributes = feature.items()
                for field, flag in [('stroke_col', 'stroked'), ('fill_color', 'filled')]:
                    value = attributes.get(field)
                    if drawn(attributes.get(flag)) and value is not None and str(value).strip(): colors[field][str(value).strip()] += 1
                if drawn(attributes.get('stroked')) and attributes.get('stroke_wid') is not None:
                    try:
                        width = float(attributes['stroke_wid'])
                        if math.isfinite(width): widths.add(width)
                    except (ValueError, TypeError): pass
                if 'original_c' in attributes: original_closed[str(attributes['original_c']).strip().casefold()] += 1
            if original_closed and set(original_closed) <= {'true', 'false', 'none', ''}:
                result['semantic_field_warnings']['original_c'] = 'drawing_path_closed_state_not_feature_code'
            if not palette:
                result['status'] = 'not_evaluable'; result['reason'] = 'original_pdf_layer_not_resolved'
                continue
            for field, values in colors.items():
                original = palette[field]
                compatible, incompatible, unknown = [], [], []
                for text in values:
                    variants = rgb_variants(text)
                    if not variants or not original: unknown.append(text)
                    elif any(max(abs(a-b) for a,b in zip(rgb, candidate)) <= .018 for rgb in variants for candidate in original): compatible.append(text)
                    else: incompatible.append(text)
                result['colors'][field] = {'verified_values': compatible, 'mismatched_values': incompatible,
                    'not_evaluable_values': unknown, 'value_counts': dict(values), 'pdf_paint_color_count': len(original),
                    'pdf_rgb_palette': [list(rgb) for rgb in sorted(original)]}
            verified_widths = [w for w in widths if any(abs(w-original) <= max(.001, abs(original)*.003) for original in palette['widths'])]
            result['widths_pt'] = {'verified_values': sorted(verified_widths), 'pdf_values': sorted(palette['widths']),
                                   'unit': 'pt', 'method': 'parsed_pdf_point_width_compatibility'}
            result['composite_stroke_palette'] = len(palette['stroke_col']) > 1
        except Exception as exc:
            result['status'] = 'not_evaluable'; result['reason'] = type(exc).__name__ + ': ' + repr(exc); report['status'] = 'partial'
            result['gdal_error'] = [gdal.GetLastErrorType(),gdal.GetLastErrorNo(),gdal.GetLastErrorMsg()]
            if 'first_error_traceback' not in report:
                import traceback
                report['first_error_traceback'] = traceback.format_exc()
    document.close()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + '.tmp')
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    temporary.replace(output)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pdf', required=True, type=Path)
    parser.add_argument('--shp-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = audit(args.pdf, args.shp_dir, args.output)
    print(json.dumps({'status': report['status'], 'layers': len(report['layers']),
        'source_color_values_incompatible': sum(len(value.get('mismatched_values', [])) for layer in report['layers'].values() for value in layer.get('colors', {}).values())}, ensure_ascii=True))
