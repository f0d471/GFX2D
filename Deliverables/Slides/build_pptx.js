// 生成 gfx2d项目讲解.pptx：封面用原生形状，其余每页一张 ../Assert/figures/ 的主图，
// 讲稿不入库。命令行给出讲稿路径时，按「## 第 N 页」切分后写进每页的演讲者备注；
// 不给时生成不带备注的版本（入库的就是这一版）。
// 依赖 pptxgenjs（npm 包，不入库）。用法见本目录 README.md。
const fs = require("fs");
const path = require("path");
const pptxgen = require("pptxgenjs");

const HERE = __dirname;
const FIG = path.join(HERE, "..", "Assert", "figures");
const OUT = path.join(HERE, "gfx2d项目讲解.pptx");
const NOTES = process.argv[2]; // 可选：讲稿路径

// 与 print.html 同页序：[图名, 页标题]
const PAGES = [
  ["task", "赛题与指标"],
  ["platform", "Ti60F225 开发板与资源"],
  ["flow", "工程组成与构建流程"],
  ["arch_current", "当前硬件架构（已上板）"],
  ["boot", "启动：从 Flash 到 main"],
  ["cpu_mem", "CPU 取指与访存"],
  ["mmio", "MMIO、驱动与中断"],
  ["axi", "AXI4 总线"],
  ["fsm", "有限状态机：以 axi_probe_master 为例"],
  ["hdmi", "HDMI 显示链路"],
  ["framebuffer", "帧缓冲、撕裂与双缓冲"],
  ["method", "加速器设计方法"],
  ["profile", "本题拆账"],
  ["blend", "Alpha 混合、线性递推与色键"],
  ["pixel_pipeline", "像素流水线"],
  ["roofline", "Roofline：访存受限"],
  ["dma", "DMA 与命令描述符"],
  ["band_cache", "片上缓存与条带合成"],
  ["arch_target", "目标硬件架构"],
  ["sw_verify", "目标软件与验证架构"],
  ["roadmap", "进度与计划"],
];

// 讲稿：每节内的软换行直接拼接（中文不加空格），空行分段
function loadNotes() {
  if (!NOTES) return null;
  const md = fs.readFileSync(NOTES, "utf8").replace(/\r\n/g, "\n");
  const notes = {};
  for (const sec of md.split(/^## /m).slice(1)) {
    const m = sec.match(/^第 (\d+) 页[^\n]*\n([\s\S]*)$/);
    if (!m) continue;
    notes[+m[1]] = m[2].trim().split(/\n\s*\n/)
      .map(p => p.split("\n").map(s => s.trim()).join(""))
      .join("\n\n");
  }
  return notes;
}

const C = {
  bg: "FAFAF7", ink: "174A7E", mute: "87929E",
  night: "13202F", cell: "1B2B3D", cellHi: "22364C", paper: "E9EEF3", sub: "8FA3B8",
  blue: "4A7FD0", red: "E8546B", green: "67BA4A", gold: "A37A1C",
};
const FONT = "Microsoft YaHei";

const notes = loadNotes();
const pres = new pptxgen();
pres.layout = "LAYOUT_WIDE"; // 13.333 x 7.5 in
pres.title = "gfx2d 项目讲解";

// ---- 封面 ----
{
  const s = pres.addSlide();
  s.background = { color: C.night };

  // 右侧像素栅格：12 x 12 个方格，越往左越淡
  const N = 12, SZ = 0.34, GAP = 0.1, X0 = 7.35, Y0 = (7.5 - (N * SZ + (N - 1) * GAP)) / 2;
  const cx = (i) => X0 + i * (SZ + GAP), cy = (j) => Y0 + j * (SZ + GAP);
  for (let j = 0; j < N; j++) for (let i = 0; i < N; i++) {
    s.addShape(pres.shapes.RECTANGLE, {
      x: cx(i), y: cy(j), w: SZ, h: SZ,
      fill: { color: C.cell, transparency: Math.round(70 - i * 5) }, line: { type: "none" },
    });
  }
  // 三块半透明色块按栅格对齐、互相叠压，示意逐层 Alpha 合成
  const block = (i, j, n, color, t) => s.addShape(pres.shapes.RECTANGLE, {
    x: cx(i), y: cy(j), w: n * SZ + (n - 1) * GAP, h: n * SZ + (n - 1) * GAP,
    fill: { color, transparency: t }, line: { type: "none" },
  });
  block(3, 2, 5, C.blue, 25);
  block(6, 5, 4, C.green, 40);
  block(5, 7, 3, C.gold, 35);

  s.addText("2026 嵌入式芯片与系统设计竞赛", {
    x: 0.9, y: 2.2, w: 6, h: 0.4, margin: 0, isTextBox: true,
    fontFace: FONT, fontSize: 14, color: C.sub, charSpacing: 2,
  });
  s.addText("gfx2d", {
    x: 0.9, y: 2.7, w: 6, h: 1.5, margin: 0, isTextBox: true,
    fontFace: "Cambria", fontSize: 88, bold: true, color: C.paper,
  });
  s.addText("2D 渲染加速引擎", {
    x: 0.9, y: 4.25, w: 6, h: 0.6, margin: 0, isTextBox: true,
    fontFace: FONT, fontSize: 26, color: C.paper,
  });
  if (notes) s.addNotes(notes[1] || "");
}

// ---- 内容页 ----
PAGES.forEach(([fig, title], k) => {
  const no = k + 2;
  const s = pres.addSlide();
  s.background = { color: C.bg };
  s.addText(title, {
    x: 0.55, y: 0.28, w: 10.5, h: 0.55, margin: 0, isTextBox: true,
    fontFace: FONT, fontSize: 22, bold: true, color: C.ink,
  });
  s.addText(String(no).padStart(2, "0"), {
    x: 11.8, y: 0.32, w: 1.0, h: 0.45, margin: 0, isTextBox: true, align: "right",
    fontFace: FONT, fontSize: 16, color: C.mute,
  });
  // 主图 1760x880（2:1），宽 12.2 in
  s.addImage({ path: path.join(FIG, fig + ".png"), x: 0.567, y: 1.05, w: 12.2, h: 6.1 });
  if (notes) {
    if (!notes[no]) throw new Error("讲稿缺第 " + no + " 页");
    s.addNotes(notes[no]);
  }
});

pres.writeFile({ fileName: OUT }).then(f => console.log("wrote " + f));
