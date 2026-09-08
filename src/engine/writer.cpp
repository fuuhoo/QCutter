#include "engine/writer.h"
#include "engine/error.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <QFile>
#include <QCoreApplication>
#include <QDir>

namespace qcutter {

fs::path tileRelPath(Scheme scheme, std::uint32_t level,
                     std::uint32_t x, std::uint32_t y_in, std::uint32_t tiles_y) {
    std::uint32_t y = (scheme == Scheme::Tms) ? (tiles_y - 1 - y_in) : y_in;
    return fs::path(std::to_string(level)) / std::to_string(x) / (std::to_string(y) + ".png");
}

void ensureOutDir(const fs::path& out) {
    std::error_code ec;
    std::experimental::filesystem::create_directories(out, ec);
    if (ec) throw CoreError::io(out.string(), ec.message());
}

static nlohmann::json manifestLevelToJson(const ManifestLevel& l) {
    return {
        {"level", l.level},
        {"width", l.width},
        {"height", l.height},
        {"tiles", l.tiles},
        {"ox", l.ox},
        {"oy", l.oy},
        {"wy", l.wy},
    };
}

void writeManifest(const fs::path& out, const Manifest& m) {
    nlohmann::json j;
    j["app"] = m.app;
    j["version"] = m.version;
    j["source"] = m.source;
    j["source_width"] = m.source_width;
    j["source_height"] = m.source_height;
    j["tile_size"] = m.tile_size;
    j["scheme"] = m.scheme;
    j["min_level"] = m.min_level;
    j["max_level"] = m.max_level;
    auto arr = nlohmann::json::array();
    for (const auto& l : m.levels) arr.push_back(manifestLevelToJson(l));
    j["levels"] = arr;
    j["total_tiles"] = m.total_tiles;
    j["bytes_written"] = m.bytes_written;

    const auto p = out / MANIFEST_NAME;
    std::ofstream f(p.string());
    if (!f) throw CoreError::io(p.string(), "open for write failed");
    f << j.dump(2);
}

void writePreviewHtml(const fs::path& out, const PreviewInfo& info) {
    // overlays: 历史/新接口有差异:
    //   - 新接口 (state_->overlaysJson): `{"overlays":[...]}`
    //   - 历史/简写: 直接 `[...]`
    // 兼容两种格式: 解析后若是 object 取 "overlays" 字段, 若是 array 直接用.
    nlohmann::json overlays = nlohmann::json::array();
    try {
        auto parsed = nlohmann::json::parse(info.overlays_json);
        if (parsed.is_array()) {
            overlays = parsed;
        } else if (parsed.is_object() && parsed.contains("overlays")) {
            overlays = parsed["overlays"];
            if (!overlays.is_array()) overlays = nlohmann::json::array();
        }
    } catch (...) { /* 保持空数组 */ }

    nlohmann::json levels = nlohmann::json::array();
    for (const auto& l : info.levels) {
        const double tw = info.tile_size;
        levels.push_back({
            {"z", l.level}, {"w", l.width}, {"h", l.height},
            {"tx", static_cast<std::uint64_t>(std::ceil(l.width / tw))},
            {"ty", static_cast<std::uint64_t>(std::ceil(l.height / tw))},
            {"ox", l.ox}, {"oy", l.oy}, {"wy", l.wy},
        });
    }
    nlohmann::json cfg = {
        {"w", info.source_w}, {"h", info.source_h},
        {"t", info.tile_size},
        {"zmin", info.zmin}, {"zmax", info.zmax},
        {"tms", info.tms},
        {"overlays", overlays},
        {"levels", levels},
    };

    static const char* kHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>QCutter 瓦片预览</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="stylesheet" href="./maplibre-gl.css">
<script src="./maplibre-gl.js"></script>
<style>
 html,body{margin:0;height:100%;background:#fafbfc}
 /* 透明棋盘格: 经典图像编辑器的透明表示, 浅灰底 + 中灰格.
    缺瓦片/未加载区域一目了然, 黑色图边也可清晰分辨.
    MapLibre 会在 #map 内创建 .maplibregl-canvas-container (默认黑底),
    我们把棋盘格放到底层, 透过 canvas 显示. */
 #map{position:absolute;inset:0}
 /* canvas 透明: 缺瓦片/未加载处露出下方棋盘格 */
 .maplibregl-canvas{outline:none}
 .maplibregl-canvas-container{background-color:transparent}
 /* 棋盘格放在最底层 (html::before) 避免被 canvas 覆盖 */
 html::before{
   content:"";position:fixed;inset:0;z-index:-1;
   background-color:#fafbfc;
   background-image:
        linear-gradient(45deg,#d8dde4 25%,transparent 25%),
        linear-gradient(-45deg,#d8dde4 25%,transparent 25%),
        linear-gradient(45deg,transparent 75%,#d8dde4 75%),
        linear-gradient(-45deg,transparent 75%,#d8dde4 75%);
   background-size:20px 20px;
   background-position:0 0,0 10px,10px -10px,-10px 0px;
 }
 #bar{position:absolute;left:0;right:0;top:0;z-index:1000;display:flex;gap:10px;align-items:center;
      padding:8px 14px;background:#171c26e6;border-bottom:1px solid #ffffff14;backdrop-filter:blur(6px)}
 #bar b{font-size:13px;color:#dfe5ef}
 #badge{font-size:11.5px;font-weight:700;color:#8fb4ff;background:#4f8cff22;border:1px solid #4f8cff44;border-radius:999px;padding:3px 10px}
 .sp{flex:1}
 #hint{font-size:11px;color:#7a8699}
 #ovp{position:absolute;top:52px;right:10px;z-index:1000;background:#171c26f2;border:1px solid #ffffff20;
      border-radius:12px;padding:8px 12px;font-size:12px;color:#cfd6e4;max-width:380px;display:flex;flex-direction:column;gap:4px;max-height:calc(100vh - 80px);overflow:auto}
 #ovp label{display:flex;gap:6px;align-items:center}
 #ovp input[type=range]{width:110px;accent-color:#4f8cff}
 #ovp input[type=text],#ovp input[type=number],#ovp select{font-family:ui-monospace,monospace}
 #ovp input[type=text]{min-width:0}
 #ovp .ov-item:first-child{border-top:none !important}
 .offline{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;color:#7a8699;font-size:13px;z-index:999}
</style></head>
<body>
<div id="bar">
 <b>QCutter 预览（MapLibre）</b>
 <span class="sp"></span>
 <span id="badge"></span>
 <span id="zlvl" style="font-size:11.5px;font-weight:700;color:#ffb4b4;background:#ff555522;border:1px solid #ff555544;border-radius:999px;padding:3px 10px;margin-left:6px"></span>
 <span id="hint">拖动 / 滚轮缩放</span>
</div>
<div id="map"></div>
<div id="diagPanel" style="position:absolute;left:10px;bottom:10px;z-index:1500;background:#171c26f2;border:1px solid #ffffff20;border-radius:10px;padding:8px 12px;font:11px/1.5 ui-monospace,monospace;color:#cfd6e4;max-width:540px;display:flex;flex-direction:column;gap:4px">
  <div style="display:flex;gap:6px;align-items:center;flex-wrap:wrap">
    <b style="color:#8fb4ff">诊断</b>
    <label style="display:flex;gap:4px;align-items:center;cursor:pointer">
      <input type="checkbox" id="toggleLocal" checked /> 显示切片
    </label>
    <label style="display:flex;gap:4px;align-items:center">
      切片透明度 <input type="range" id="localOpacity" min="0" max="100" value="100" style="width:90px;accent-color:#4f8cff" />
    </label>
    <button id="recenterBtn" style="margin-left:auto;background:#4f8cff22;border:1px solid #4f8cff44;color:#8fb4ff;border-radius:6px;padding:2px 8px;cursor:pointer;font:11px">↺ 回到切片范围</button>
  </div>
  <div style="display:grid;grid-template-columns:auto 1fr;gap:2px 10px">
    <span style="color:#7a8699">鼠标位置</span><span id="mouseLL" style="color:#dfe5ef">—</span>
    <span style="color:#7a8699">鼠标瓦片</span><span id="mouseTile" style="color:#dfe5ef">—</span>
    <span style="color:#7a8699">当前 Z</span><span id="currentZ" style="color:#dfe5ef">—</span>
  </div>
  <pre id="diagInfo" style="margin:0;white-space:pre-wrap;color:#a8b3c5;font:10.5px/1.5 ui-monospace,monospace"></pre>
</div>
<script>
const CFG = __CFG__;

function showErr(msg){
  let el = document.getElementById('errBox');
  if(!el){ el = document.createElement('div'); el.id='errBox';
    el.style.cssText='position:absolute;left:10px;bottom:10px;right:10px;z-index:2000;background:#3a1010f2;color:#ffb4b4;border:1px solid #ff555555;border-radius:10px;padding:10px;font:12px/1.6 monospace;white-space:pre-wrap;max-height:40vh;overflow:auto';
    document.body.appendChild(el); }
  el.textContent = (el.textContent? el.textContent+'\n':'') + msg;
}
window.addEventListener('error', e => showErr('[error] '+e.message+' @'+(e.filename||'')+':'+(e.lineno||'?')));
window.addEventListener('unhandledrejection', e => showErr('[promise] '+String(e.reason)));

if (typeof maplibregl === 'undefined') {
  document.getElementById('map').innerHTML =
    '<div class="offline">MapLibre GL JS 未能加载。瓦片本身完好，可直接浏览 {z}/{x}/{y}.png 目录。</div>';
} else {
try {
  document.getElementById('badge').textContent =
    'Z' + CFG.zmin + '–Z' + CFG.zmax + ' · ' + (CFG.tms?'TMS':'XYZ') + ' · ' + CFG.t + 'px';

  function tileLL(x, y, z) {
    const n = Math.pow(2, z);
    const lon = x / n * 360 - 180;
    const lat = Math.atan(Math.sinh(Math.PI * (1 - 2 * y / n))) * 180 / Math.PI;
    return [lon, lat];
  }

  const B = CFG.levels.find(function(l) { return l.z === CFG.zmax; }) || CFG.levels[CFG.levels.length - 1] || {ox:0, oy:0, tx:1, ty:1, z:CFG.zmax};
  var bo = { ox: B.ox || 0, oy: B.oy || 0 };
  var sw = tileLL(bo.ox, bo.oy + B.ty, B.z);
  var ne = tileLL(bo.ox + B.tx, bo.oy, B.z);
  var bounds = [sw, ne];

  var diag = {
    bounds: bounds,
    center: [(sw[0] + ne[0]) / 2, (sw[1] + ne[1]) / 2],
    ox: bo.ox, oy: bo.oy,
    tx: B.tx, ty: B.ty, z: B.z,
    wy: B.wy,
    scheme: CFG.tms ? 'tms' : 'xyz',
    localZmin: CFG.zmin, localZmax: CFG.zmax,
    overlays: (CFG.overlays||[]).map(function(o){ return {name:o.name, on:!!o.on, below:o.below!==false, zmin:o.zmin, zmax:o.zmax, tms:o.tms}; })
  };
  document.getElementById('diagInfo').textContent =
    '切片范围 sw=' + JSON.stringify({lon: +sw[0].toFixed(4), lat: +sw[1].toFixed(4)}) +
    '  ne=' + JSON.stringify({lon: +ne[0].toFixed(4), lat: +ne[1].toFixed(4)}) +
    '\n切片 Z[' + CFG.zmin + ',' + CFG.zmax + ']  scheme=' + (CFG.tms?'TMS':'XYZ') +
    '  当前 L.B=' + bo.ox + ',' + bo.oy + '  size=' + B.tx + 'x' + B.ty + ' (z=' + B.z + ')' +
    '\n底图：' + (CFG.overlays||[]).map(function(o){return (o.on?'✓':'✗')+' '+o.name+' [z'+o.zmin+'-'+o.zmax+' '+(o.tms?'TMS':'XYZ')+']';}).join(' | ');

  // 空 style 的 canvas 在 MapLibre 默认是黑色. 在 style 中显式加入一个 background
  // layer 覆盖默认底色为浅灰 (#fafbfc). CSS 已在 #map 上叠了 20px 棋盘格.
  var map = new maplibregl.Map({
    container: 'map',
    style: {
      version: 8,
      sources: {},
      layers: [{
        id: 'checker-bg',
        type: 'background',
        paint: { 'background-color': '#fafbfc' }
      }]
    },
    center: [(sw[0] + ne[0]) / 2, (sw[1] + ne[1]) / 2],
    zoom: CFG.zmin,
    maxZoom: CFG.zmax,
    minZoom: CFG.zmin,
    attributionControl: true
  });

  map.addControl(new maplibregl.NavigationControl(), 'bottom-right');
  map.addControl(new maplibregl.ScaleControl({imperial: false}), 'bottom-left');

  map.on('load', function() {
    map.addSource('local-tiles', {
      type: 'raster',
      tiles: ['{z}/{x}/{y}.png'],
      tileSize: CFG.t,
      maxzoom: CFG.zmax,
      minzoom: CFG.zmin,
      scheme: CFG.tms ? 'tms' : 'xyz'
    });

    map.addLayer({
      id: 'local-tiles-layer',
      type: 'raster',
      source: 'local-tiles',
      paint: { 'raster-resampling': 'linear' }
    });

    map.fitBounds(bounds, {padding: 24});

    function updateZoom() {
      var z = map.getZoom();
      var rounded = Math.round(z * 4) / 4;
      document.getElementById('zlvl').textContent = '当前 Z' + (rounded % 1 ? rounded.toFixed(2) : rounded);
      document.getElementById('currentZ').textContent = rounded.toFixed(2) + ' (整数 Z = ' + Math.round(z) + ')';
    }
    map.on('zoom', updateZoom);
    updateZoom();

    map.on('mousemove', function(e) {
      var lon = e.lngLat.lng, lat = e.lngLat.lat;
      var z = Math.round(map.getZoom());
      var n = Math.pow(2, z);
      var x = Math.floor((lon + 180) / 360 * n);
      var y = Math.floor((1 - Math.log(Math.tan(lat * Math.PI / 180) + 1 / Math.cos(lat * Math.PI / 180)) / Math.PI) / 2 * n);
      document.getElementById('mouseLL').textContent = lon.toFixed(5) + '°E, ' + lat.toFixed(5) + '°N';
      document.getElementById('mouseTile').textContent = 'z=' + z + '  x=' + x + '  y=' + y;
    });
    document.getElementById('toggleLocal').addEventListener('change', function(e) {
      var vis = e.target.checked ? 'visible' : 'none';
      map.setLayoutProperty('local-tiles-layer', 'visibility', vis);
    });
    document.getElementById('localOpacity').addEventListener('input', function(e) {
      var op = +e.target.value / 100;
      map.setPaintProperty('local-tiles-layer', 'raster-opacity', op);
    });
    document.getElementById('recenterBtn').addEventListener('click', function() {
      map.fitBounds(bounds, {padding: 24});
    });

    var ovs = (CFG.overlays || []).filter(function(o) { return o && o.on && o.tpl; });
    var items = [];
    ovs.forEach(function(o, i) {
      // subs 在桌面端已规范化为字符串数组 (例如 ["0","1",...,"7"]).
      // 兼容旧 JSON (字符串如 "0 1 2 ...") 和空 subs.
      var subs;
      if (Array.isArray(o.subs)) {
        subs = o.subs.map(String);
        if (subs.length === 0) subs = [''];
      } else if (o.subs && String(o.subs).length) {
        // 旧格式: 字符串, 去掉空白, 逐字符切分
        subs = String(o.subs).split('').filter(function(c){ return c && !/\s/.test(c); });
        if (subs.length === 0) subs = [''];
      } else {
        subs = [''];
      }
      var tiles = subs.map(function(s) {
        var t = String(o.tpl).replace(/\{s\}/g, s);
        return t.replace(/\{([a-zA-Z0-9]+)\}/g, function(_, k) {
          return o[k] != null ? o[k] : ('{' + k + '}');
        });
      });
      var layerId = 'online-' + i;
      var sourceId = 'online-src-' + i;
      map.addSource(sourceId, {
        type: 'raster',
        tiles: tiles,
        tileSize: 256,
        scheme: o.tms ? 'tms' : 'xyz'
      });
      var layerSpec = {
        id: layerId,
        type: 'raster',
        source: sourceId,
        paint: {
          'raster-opacity': typeof o.opacity === 'number' ? o.opacity : 1,
          'raster-resampling': 'nearest'
        }
      };
      if (typeof o.zmin === 'number') layerSpec.minzoom = o.zmin;
      if (typeof o.zmax === 'number') layerSpec.maxzoom = o.zmax;
      map.addLayer(layerSpec, o.below === false ? null : 'local-tiles-layer');
      items.push({
        name: o.name || ('layer' + i),
        layerId: layerId,
        defaultOn: o.below !== false
      });
    });

    if (items.length) {
      items.forEach(function(it) {
        map.setLayoutProperty(it.layerId, 'visibility',
          it.defaultOn ? 'visible' : 'none');
      });
      // 在线底图控制面板: 只读 URL, 不可编辑 (配置由桌面端控制).
      // 每行: 显隐 checkbox + 名称 + 透明度滑块 + URL 模板(只读 label).
      // 注意: 这里的 raw/ovs/items 三个列表必须保持一一对应, 即 raw[i] / ovs[i]
      // / items[i] 描述同一个 overlay. ovs 在前面已经过滤了 on && tpl, raw
      // 只过滤了 tpl (为了让用户能取消勾选一个底图), 索引错位会让取消后点击
      // checkbox 报 "items[i] is undefined" / "Cannot read layerId". 统一用
      // ovs 的索引作 data-i, 保证 items[] 一定有对应项.
      var p = document.createElement('div');
      p.id = 'ovp';
      p.innerHTML = '<b style="font-size:12px">在线底图</b>' + ovs.map(function(o, i) {
        var checked = items[i] && items[i].defaultOn ? ' checked' : '';
        var tplDisp = (o.tpl || '').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        return (
          '<div class="ov-item" style="display:flex;flex-direction:column;gap:4px;padding:6px 0;border-top:1px solid #ffffff14">' +
            '<div style="display:flex;gap:6px;align-items:center;flex-wrap:wrap">' +
              '<label style="display:flex;gap:4px;align-items:center;cursor:pointer;flex:0 0 auto">' +
                '<input type="checkbox" data-i="' + i + '" data-act="vis"' + checked + '> ' +
                '<b>' + (o.name || ('layer' + i)) + '</b>' +
              '</label>' +
              '<label style="font-size:10.5px;color:#7a8699">透明' +
                '<input type="range" min="10" max="100" value="' +
                  (typeof o.opacity === 'number' ? Math.round(o.opacity*100) : 100) +
                  '" data-i="' + i + '" data-act="op" style="width:90px;margin-left:4px;accent-color:#4f8cff">' +
              '</label>' +
              '<span style="font-size:10.5px;color:#7a8699">Z ' +
                ((typeof o.zmin === 'number') ? o.zmin : 0) + '–' +
                ((typeof o.zmax === 'number') ? o.zmax : 22) +
                ' · ' + (o.tms ? 'TMS' : 'XYZ') +
              '</span>' +
            '</div>' +
            '<div style="font:10.5px/1.4 ui-monospace,monospace;color:#a8b3c5;' +
                       'background:#0f1320;border:1px solid #ffffff10;border-radius:4px;' +
                       'padding:3px 6px;word-break:break-all" ' +
                  'title="只读, 不可在浏览器内编辑. 请在桌面端 QCutter 设置里修改">' +
              tplDisp +
            '</div>' +
          '</div>'
        );
      }).join('');
      document.body.appendChild(p);

      // 显隐 checkbox
      p.querySelectorAll('input[data-act="vis"]').forEach(function(el) {
        el.onchange = function(e) {
          var it = items[+e.target.dataset.i];
          if (!it) return;
          map.setLayoutProperty(it.layerId, 'visibility', e.target.checked ? 'visible' : 'none');
        };
      });
      // 透明度滑块
      p.querySelectorAll('input[data-act="op"]').forEach(function(el) {
        el.oninput = function(e) {
          var it = items[+e.target.dataset.i];
          if (!it) return;
          map.setPaintProperty(it.layerId, 'raster-opacity', +e.target.value / 100);
        };
      });
    }

    setTimeout(function() {
      var source = map.getSource('local-tiles');
      if (!source) showErr('[warn] local-tiles source 未创建');
    }, 1200);
  });
} catch(err){
  showErr('[init] ' + (err && err.stack ? err.stack : String(err)));
}
}
</script></body></html>
)HTML";

    std::string html = kHtml;
    const std::string cfg_str = cfg.dump();
    const std::string token = "__CFG__";
    const auto pos = html.find(token);
    if (pos != std::string::npos) {
        html.replace(pos, token.size(), cfg_str);
    }
    const auto p = out / PREVIEW_HTML_NAME;
    std::ofstream f(p.string());
    if (!f) throw CoreError::io(p.string(), "open for write failed");
    f << html;

    // 写入 maplibre 静态资源 (从应用 exe 旁边或 assets/ 目录读取)
    auto writeIfDiff = [&](const fs::path& p, const QByteArray& data) {
        const auto len = static_cast<std::size_t>(data.size());
        if (fs::exists(p)) {
            std::ifstream in(p.string(), std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            const auto& existing = ss.str();
            if (existing.size() == len && std::memcmp(existing.data(), data.constData(), len) == 0) {
                return;
            }
        }
        std::ofstream o(p.string(), std::ios::binary);
        o.write(data.constData(), static_cast<std::streamsize>(len));
    };
    const QString exeDir = QCoreApplication::applicationDirPath();
    // 顺序探测候选路径
    auto load = [&](const QString& fname) -> QByteArray {
        const QStringList candidates = {
            exeDir + "/" + fname,
            exeDir + "/assets/maplibre/" + fname,
            exeDir + "/../share/qcutter/maplibre/" + fname,
        };
        for (const auto& c : candidates) {
            QFile f(c);
            if (f.exists() && f.open(QIODevice::ReadOnly)) return f.readAll();
        }
        return {};
    };
    writeIfDiff(out / "maplibre-gl.js",  load("maplibre-gl.js"));
    writeIfDiff(out / "maplibre-gl.css", load("maplibre-gl.css"));
}

} // namespace qcutter