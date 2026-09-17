// 配图连线。每张图在 window.LINKS 里登记连线，页面加载后统一绘制。
// L(from, to, opt)
//   from / to：元素 id，或 [x, y] 绝对坐标（相对 #fig）
//   opt.fs / opt.ts：起止边 'r' 'l' 't' 'b'，默认按相对位置推断
//   opt.ff / opt.tf：起止点在边上的位置比例，默认 0.5
//   opt.r：路由 's' 直线，'hv' 先横后竖，'vh' 先竖后横，'hvh' 横竖横（opt.m 为中间 x），'vhv'（opt.m 为中间 y）
//   opt.via：[[x, y], ...] 中间点，给出时忽略 r
//   opt.c 颜色，opt.w 线宽，opt.d 虚线，opt.h 箭头 'end' 'both' 'none'
//   opt.t 标签文字，opt.tx / opt.ty 标签绝对位置（缺省为最长线段中点），opt.tc 标签颜色
window.LINKS = [];
function L(from, to, opt) { window.LINKS.push([from, to, opt || {}]); }

(function () {
  function box(fig, id) {
    const e = document.getElementById(id);
    if (!e) throw new Error('missing node ' + id);
    const r = e.getBoundingClientRect(), f = fig.getBoundingClientRect();
    return { x: r.left - f.left, y: r.top - f.top, w: r.width, h: r.height };
  }
  function side(b, s, k) {
    if (s === 'r') return [b.x + b.w, b.y + b.h * k];
    if (s === 'l') return [b.x, b.y + b.h * k];
    if (s === 't') return [b.x + b.w * k, b.y];
    return [b.x + b.w * k, b.y + b.h];
  }
  function guess(a, b) {
    const ax = a.x + a.w / 2, ay = a.y + a.h / 2, bx = b.x + b.w / 2, by = b.y + b.h / 2;
    if (Math.abs(bx - ax) * a.h > Math.abs(by - ay) * a.w) return bx > ax ? ['r', 'l'] : ['l', 'r'];
    return by > ay ? ['b', 't'] : ['t', 'b'];
  }
  function draw() {
    const fig = document.getElementById('fig');
    const W = fig.clientWidth, H = fig.clientHeight;
    const ns = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(ns, 'svg');
    svg.setAttribute('class', 'links'); svg.setAttribute('width', W); svg.setAttribute('height', H);
    const defs = document.createElementNS(ns, 'defs'); svg.appendChild(defs);
    fig.appendChild(svg);
    const marks = {};
    function marker(c) {
      if (marks[c]) return marks[c];
      const id = 'm' + Object.keys(marks).length;
      const m = document.createElementNS(ns, 'marker');
      m.setAttribute('id', id); m.setAttribute('viewBox', '0 0 10 10'); m.setAttribute('refX', '8.5'); m.setAttribute('refY', '5');
      m.setAttribute('markerWidth', '7'); m.setAttribute('markerHeight', '7'); m.setAttribute('orient', 'auto-start-reverse');
      m.setAttribute('markerUnits', 'userSpaceOnUse');
      m.setAttribute('markerWidth', '14'); m.setAttribute('markerHeight', '14');
      const p = document.createElementNS(ns, 'path'); p.setAttribute('d', 'M0,0 L10,5 L0,10 z'); p.setAttribute('fill', c);
      m.appendChild(p); defs.appendChild(m); marks[c] = id; return id;
    }
    for (const [from, to, o] of window.LINKS) {
      let p1, p2;
      const A = Array.isArray(from) ? null : box(fig, from), B = Array.isArray(to) ? null : box(fig, to);
      let fs = o.fs, ts = o.ts;
      if (A && B && (!fs || !ts)) { const g = guess(A, B); fs = fs || g[0]; ts = ts || g[1]; }
      p1 = A ? side(A, fs || 'r', o.ff == null ? 0.5 : o.ff) : from;
      p2 = B ? side(B, ts || 'l', o.tf == null ? 0.5 : o.tf) : to;
      let pts = [p1];
      if (o.via) pts = pts.concat(o.via);
      else if (o.r === 'hv') pts.push([p2[0], p1[1]]);
      else if (o.r === 'vh') pts.push([p1[0], p2[1]]);
      else if (o.r === 'hvh') { const m = o.m == null ? (p1[0] + p2[0]) / 2 : o.m; pts.push([m, p1[1]], [m, p2[1]]); }
      else if (o.r === 'vhv') { const m = o.m == null ? (p1[1] + p2[1]) / 2 : o.m; pts.push([p1[0], m], [p2[0], m]); }
      pts.push(p2);
      const c = o.c || '#33445A';
      const path = document.createElementNS(ns, 'path');
      path.setAttribute('d', pts.map((p, i) => (i ? 'L' : 'M') + p[0].toFixed(1) + ',' + p[1].toFixed(1)).join(' '));
      path.setAttribute('fill', 'none'); path.setAttribute('stroke', c); path.setAttribute('stroke-width', o.w || 2.5);
      path.setAttribute('stroke-linejoin', 'round');
      if (o.d) path.setAttribute('stroke-dasharray', o.d === true ? '8 6' : o.d);
      const h = o.h || 'end';
      if (h === 'end' || h === 'both') path.setAttribute('marker-end', 'url(#' + marker(c) + ')');
      if (h === 'both') path.setAttribute('marker-start', 'url(#' + marker(c) + ')');
      svg.appendChild(path);
      if (o.t) {
        let tx = o.tx, ty = o.ty;
        if (tx == null) {
          let best = 0, bi = 0;
          for (let i = 0; i + 1 < pts.length; i++) {
            const d = Math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]);
            if (d > best) { best = d; bi = i; }
          }
          tx = (pts[bi][0] + pts[bi + 1][0]) / 2; ty = (pts[bi][1] + pts[bi + 1][1]) / 2;
        }
        const d = document.createElement('div');
        d.className = 'lbl bg'; d.style.left = tx + 'px'; d.style.top = ty + 'px';
        d.style.transform = 'translate(-50%, -50%)'; d.style.color = o.tc || c;
        if (o.tb) d.style.fontWeight = '700';
        d.innerHTML = o.t; fig.appendChild(d);
      }
    }
    document.body.setAttribute('data-ready', '1');
  }
  window.addEventListener('load', () => { if (document.fonts) document.fonts.ready.then(draw); else draw(); });
})();

// 时序波形。wave({x, y, cw, rh, nw, n, rows, hl, color})
//   rows：{n:'ACLK', clk:true} | {n:'VALID', b:'0011100'} | {n:'DATA', d:['', 'A', 'A', '']}
//   b 串里 '0' 低、'1' 高、'.' 延续上一拍；d 数组里 '' 表示无效（画中线），相同值合并成一段
//   hl：高亮的拍号数组（握手成立的时钟沿之后那一拍），color：波形颜色
function wave(o) {
  const ns = 'http://www.w3.org/2000/svg';
  const n = o.n, cw = o.cw || 60, rh = o.rh || 46, nw = o.nw || 150;
  const W = nw + n * cw + 10, H = o.rows.length * rh + 10;
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('width', W); svg.setAttribute('height', H);
  svg.style.position = 'absolute'; svg.style.left = o.x + 'px'; svg.style.top = o.y + 'px'; svg.style.overflow = 'visible';
  const col = o.color || '#2F4B7C';
  function el(t, a, txt) { const e = document.createElementNS(ns, t); for (const k in a) e.setAttribute(k, a[k]); if (txt != null) e.textContent = txt; svg.appendChild(e); return e; }
  (o.hl || []).forEach(i => el('rect', { x: nw + i * cw, y: 0, width: cw, height: H - 6, fill: '#FFE9B8', opacity: .75 }));
  for (let i = 0; i <= n; i++) el('line', { x1: nw + i * cw, y1: 0, x2: nw + i * cw, y2: H - 6, stroke: '#D9DEE5', 'stroke-width': 1, 'stroke-dasharray': '3 4' });
  o.rows.forEach((r, k) => {
    const y0 = k * rh + 8, hi = y0 + 6, lo = y0 + rh - 16, mid = (hi + lo) / 2;
    el('text', { x: nw - 12, y: mid + 7, 'text-anchor': 'end', 'font-size': 19, 'font-weight': 700, fill: '#1D2733', 'font-family': 'Consolas, monospace' }, r.n);
    if (r.clk) {
      let d = 'M' + nw + ',' + lo;
      for (let i = 0; i < n; i++) { const x = nw + i * cw; d += ' L' + x + ',' + hi + ' L' + (x + cw / 2) + ',' + hi + ' L' + (x + cw / 2) + ',' + lo + ' L' + (x + cw) + ',' + lo; }
      el('path', { d, fill: 'none', stroke: '#5D6976', 'stroke-width': 2 });
    } else if (r.b) {
      let v = r.b[0] === '1' ? 1 : 0, d = 'M' + nw + ',' + (v ? hi : lo);
      for (let i = 0; i < n; i++) {
        const c = r.b[i] === '.' || r.b[i] == null ? v : (r.b[i] === '1' ? 1 : 0);
        const x = nw + i * cw;
        if (c !== v) { d += ' L' + (x + 4) + ',' + (c ? hi : lo); v = c; }
        d += ' L' + (x + cw) + ',' + (v ? hi : lo);
      }
      el('path', { d, fill: 'none', stroke: r.c || col, 'stroke-width': 2.5 });
    } else if (r.d) {
      let i = 0;
      while (i < n) {
        let j = i; while (j + 1 < n && r.d[j + 1] === r.d[i]) j++;
        const x1 = nw + i * cw, x2 = nw + (j + 1) * cw;
        if (r.d[i] === '' || r.d[i] == null) el('line', { x1, y1: mid, x2, y2: mid, stroke: '#9AA3AD', 'stroke-width': 2 });
        else {
          el('path', { d: `M${x1},${mid} L${x1 + 6},${hi} L${x2 - 6},${hi} L${x2},${mid} L${x2 - 6},${lo} L${x1 + 6},${lo} Z`, fill: r.f || '#DEE8F7', stroke: r.c || col, 'stroke-width': 2 });
          el('text', { x: (x1 + x2) / 2, y: mid + 7, 'text-anchor': 'middle', 'font-size': 18, fill: '#1D2733', 'font-family': 'Consolas, monospace' }, r.d[i]);
        }
        i = j + 1;
      }
    }
  });
  document.getElementById('fig').appendChild(svg);
}
