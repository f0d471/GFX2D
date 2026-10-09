// 生成 gfx2d项目讲解.pptx（第四版，2026-10-05）：简约学术风，16:9。
// 版式：深底封面/结尾 + 白底内容页；内容页 = 顶部标题 + 右上章节标签 + 图/表 + 要点 + 页脚页码。
// 图取自 ../Assert/figures/report/（2 倍像素导出）与 ../Assert/{photos,measurements}/；
// 全部数字出自 ../Report/数据台账.md。
// 依赖 pptxgenjs（npm 包，不入库，用法见 README.md）。讲稿不入库，本脚本不写备注。
const path = require("path");
const pptxgen = require("pptxgenjs");
const { applyTheme } = require(path.join(
  process.env.SKILL_DIR,
  "scripts", "apply_theme.js"));

const HERE = __dirname;
const FIG = path.join(HERE, "..", "Assert", "figures", "report");
const OUT = path.join(HERE, "gfx2d项目讲解.pptx");

// ---- 主题 ----
const INK = "1F2937", DEEP = "16324F", BLUE = "174A7E", LBLUE = "4A7FD0",
      MUTE = "64748B", CARD = "F4F7FA", LINE = "D9E2EA",
      WHITE = "FFFFFF", ICE = "C9DCEF";
const FONT = "Microsoft YaHei";

const THEME = {
  name: "GFX2D Academic",
  headFontFace: FONT,
  bodyFontFace: FONT,
  colors: {
    dk1: INK, lt1: WHITE, dk2: DEEP, lt2: CARD,
    accent1: BLUE, accent2: LBLUE, accent3: "A37A1C",
    accent4: MUTE, accent5: ICE, accent6: LINE,
    hlink: BLUE, folHlink: MUTE,
  },
};

const pres = new pptxgen();
pres.layout = "LAYOUT_WIDE"; // 13.333 x 7.5
pres.author = "GFX2D 项目组";
pres.title = "GFX2D：基于 RISC-V 与 FPGA 异构架构的 2D 图形渲染加速引擎";

// ---- 布局 ----
pres.defineSlideMaster({
  title: "COVER",
  background: { color: DEEP },
  objects: [],
});
pres.defineSlideMaster({
  title: "CONTENT",
  background: { color: WHITE },
  objects: [
    { placeholder: { options: { name: "title", x: 0.55, y: 0.38, w: 9.2,
      h: 0.66, fontFace: FONT, fontSize: 25, bold: true, color: DEEP,
      align: "left", valign: "middle", margin: 0 }, text: "" } },
    { text: { text: "GFX2D · 2026 全国大学生嵌入式芯片与系统设计竞赛 · 赛题二",
      options: { x: 0.55, y: 7.08, w: 6.5, h: 0.3, fontFace: FONT, fontSize: 8.5,
        color: MUTE, align: "left", margin: 0 } } },
  ],
  slideNumber: { x: 12.55, y: 7.08, w: 0.5, h: 0.3, fontFace: FONT,
    fontSize: 9, color: MUTE, align: "right" },
});
pres.defineSlideMaster({
  title: "DARK",
  background: { color: DEEP },
  objects: [],
});

// ---- 公共小件 ----
function pageTitle(slide, text) {
  slide.addText(text, { placeholder: "title" });
}
function sectionTag(slide, text) {
  slide.addText(text, { x: 9.9, y: 0.5, w: 2.88, h: 0.34, fontFace: FONT,
    fontSize: 10.5, color: MUTE, align: "right", margin: 0, isTextBox: true });
}
// 要点列表：条目 [关键词, 说明]，渲染为「关键词　说明」，关键词深蓝加粗
function bullets(slide, items, opt) {
  const runs = [];
  items.forEach(([k, v], i) => {
    const last = i === items.length - 1;
    if (k) {
      runs.push({ text: k + "　", options: { bold: true, color: BLUE,
        breakLine: false } });
      runs.push({ text: v, options: { color: INK, breakLine: !last } });
    } else {
      runs.push({ text: v, options: { color: INK, breakLine: !last } });
    }
  });
  slide.addText(runs, Object.assign({
    fontFace: FONT, fontSize: 12.5, align: "left", valign: "top",
    margin: 0, isTextBox: true, lineSpacingMultiple: 1.18,
    paraSpaceAfter: 10,
  }, opt));
}
// 大数字卡
function statCard(slide, x, y, w, h, num, label, sub) {
  slide.addShape("roundRect", { x, y, w, h, rectRadius: 0.06,
    fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
  slide.addText(num, { x: x + 0.18, y: y + 0.16, w: w - 0.36, h: h * 0.46,
    fontFace: FONT, fontSize: 32, bold: true, color: BLUE, align: "left",
    margin: 0, isTextBox: true });
  slide.addText([
    { text: label, options: { bold: true, color: INK, breakLine: true } },
    { text: sub, options: { color: MUTE } },
  ], { x: x + 0.18, y: y + h * 0.52, w: w - 0.36, h: h * 0.44,
    fontFace: FONT, fontSize: 11, align: "left", valign: "top", margin: 0,
    isTextBox: true, lineSpacingMultiple: 1.15 });
}
// 图片（含极浅描边，区分白底图幅）
function figure(slide, file, x, y, w, h) {
  slide.addImage({ path: file, x, y, w, h });
  slide.addShape("rect", { x, y, w, h, fill: { type: "none" },
    line: { color: LINE, width: 0.75 } });
}
// 图注
function caption(slide, text, x, y, w) {
  slide.addText(text, { x, y, w, h: 0.3, fontFace: FONT, fontSize: 9.5,
    color: MUTE, align: "center", margin: 0, isTextBox: true });
}

// ---- 各页 ----

// P1 封面
{
  const s = pres.addSlide({ masterName: "COVER" });
  s.addText("GFX2D", { x: 0.9, y: 1.7, w: 11.5, h: 1.3, fontFace: FONT,
    fontSize: 66, bold: true, color: WHITE, margin: 0, isTextBox: true });
  s.addText("基于 RISC-V 与 FPGA 异构架构的 2D 图形渲染加速引擎",
    { x: 0.92, y: 3.05, w: 11.5, h: 0.6, fontFace: FONT, fontSize: 21,
      color: ICE, margin: 0, isTextBox: true });
  s.addText("2026 全国大学生嵌入式芯片与系统设计竞赛 · 易灵思赛道 赛题二 · Ti60F225",
    { x: 0.92, y: 3.78, w: 11.5, h: 0.4, fontFace: FONT, fontSize: 12.5,
      color: "8FA8C4", margin: 0, isTextBox: true });
  const stats = [
    ["60 FPS", "普通游戏短测帧率"],
    ["24×", "对纯软件渲染加速"],
    ["1891 / 2237", "N_max：16×16 与 8×8"],
    ["0 掉帧", "3600 场确认"],
  ];
  stats.forEach(([n, l], i) => {
    const x = 0.92 + i * 2.95;
    s.addText(n, { x, y: 5.3, w: 2.8, h: 0.55, fontFace: FONT, fontSize: 24,
      bold: true, color: WHITE, margin: 0, isTextBox: true });
    s.addText(l, { x, y: 5.88, w: 2.8, h: 0.35, fontFace: FONT, fontSize: 11,
      color: "8FA8C4", margin: 0, isTextBox: true });
  });
}

// P2 赛题要求与本作品
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "赛题要求与本作品");
  sectionTag(s, "01 · 赛题与结果");
  const reqs = [
    ["基础 ①", "BitBlt 引擎解析渲染指令，块拷贝与纯色填充，HDMI ≥ 640×480@60"],
    ["基础 ②", "加速器经总线直访内存，合理使用突发传输与内部 FIFO"],
    ["基础 ③", "C 驱动配置寄存器，双缓冲防撕裂；屏上实时对比软/硬帧率"],
    ["高阶 ①", "硬件 Alpha 混合，用 DSP/硬件乘法器实现"],
    ["高阶 ②", "颜色键控去背（Color Keying）"],
    ["高阶 ③", "高负载游戏 Demo，60 FPS 下挑战同屏 Sprite 数量极限"],
  ];
  reqs.forEach(([k, v], i) => {
    const y = 1.32 + i * 0.92;
    s.addShape("roundRect", { x: 0.55, y, w: 6.75, h: 0.8, rectRadius: 0.05,
      fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
    s.addText([
      { text: k + "　", options: { bold: true, color: BLUE, breakLine: false } },
      { text: v, options: { color: INK } },
    ], { x: 0.75, y: y + 0.09, w: 6.4, h: 0.62, fontFace: FONT, fontSize: 11.5,
      align: "left", valign: "middle", margin: 0, isTextBox: true });
  });
  s.addShape("roundRect", { x: 7.75, y: 1.32, w: 5.03, h: 5.42, rectRadius: 0.06,
    fill: { color: DEEP } });
  s.addText("本作品", { x: 8.1, y: 1.66, w: 4.3, h: 0.45, fontFace: FONT,
    fontSize: 17, bold: true, color: WHITE, margin: 0, isTextBox: true });
  const resp = [
    ["平台", "Ti60F225 + Sapphire RISC-V 软核（100 MHz）"],
    ["硬件", "自研渲染引擎与扫描输出，AXI4 主机"],
    ["软件", "MYRIAD 弹幕游戏 + PC/板上双目标平台"],
    ["闭环", "六项要求全部实现并上板实测"],
    ["等价", "同帧 CRC 证明软硬逐像素一致"],
    ["极限", "N_max 1891 / 2237 颗，3600 场零掉帧"],
  ];
  const runs = [];
  resp.forEach(([k, v], i) => {
    runs.push({ text: k + "　", options: { bold: true, color: "9FC0E8",
      breakLine: false } });
    runs.push({ text: v, options: { color: "E8EEF5",
      breakLine: i !== resp.length - 1 } });
  });
  s.addText(runs, { x: 8.1, y: 2.25, w: 4.35, h: 4.3, fontFace: FONT,
    fontSize: 12, valign: "top", margin: 0, isTextBox: true,
    lineSpacingMultiple: 1.22, paraSpaceAfter: 14 });
}

// P3 结果总览
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "结果总览");
  sectionTag(s, "01 · 赛题与结果");
  statCard(s, 0.55, 1.45, 3.0, 2.1, "60 FPS", "普通游戏短测",
    "SW9 软件对照 2.5 FPS");
  statCard(s, 3.75, 1.45, 3.0, 2.1, "24×", "软硬件加速比",
    "真实游戏负载，板上实测");
  statCard(s, 6.95, 1.45, 3.0, 2.1, "1891/2237", "直通 / TEX N_max",
    "条带4KB：424/505；CPU受限");
  statCard(s, 10.15, 1.45, 2.63, 2.1, "68.2%", "XLR 占用",
    "DSP 10.0%，RAM 63.7%");
  s.addShape("roundRect", { x: 0.55, y: 3.95, w: 12.23, h: 1.5,
    rectRadius: 0.06, fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
  s.addText([
    { text: "加速而不改画　", options: { bold: true, color: BLUE, breakLine: false } },
    { text: "软件执行器与硬件流水线按同一份逐像素规格实现，板上同帧 CRC 对比证明两者逐像素等价；通关录像 9208 帧与 PC 参考回放逐行一致。",
      options: { color: INK } },
  ], { x: 0.85, y: 4.14, w: 11.6, h: 1.1, fontFace: FONT, fontSize: 13,
    valign: "middle", margin: 0, isTextBox: true, lineSpacingMultiple: 1.25 });
  s.addText([
    { text: "Flash 交付链（SW9）　", options: { bold: true, color: BLUE, breakLine: false } },
    { text: "已完成镜像合成、应用写入、读回与自启动；本轮 G4 通过 JTAG 验证，未更新 Flash。",
      options: { color: INK } },
  ], { x: 0.85, y: 5.75, w: 11.6, h: 0.9, fontFace: FONT, fontSize: 13,
    valign: "middle", margin: 0, isTextBox: true, lineSpacingMultiple: 1.25 });
}

// P4 系统总体架构
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "系统总体架构：三个总线主机共享 DDR3");
  sectionTag(s, "02 · 硬件系统");
  figure(s, path.join(FIG, "rep_system.png"), 0.55, 1.32, 7.3, 5.2);
  caption(s, "系统总体架构（Sapphire SoC + GFX2D 引擎 + 扫描输出，经 Axi_Mux 共享 DDR3）",
    0.55, 6.56, 7.3);
  bullets(s, [
    ["总线", "CPU、渲染引擎、扫描输出三主机经 Axi_Mux（三进一出，128 位）共享 DDR3"],
    ["防饿死", "axi_mux_guard 保护处理器通路，地址等待读 ≤ 301、写 ≤ 311 周期"],
    ["寄存器", "控制/状态/PERF 计数器挂 APB 总线（0xF810_0000 起），完成与错误经中断上报"],
    ["负载", "Sapphire（VexRiscv RV32IM）100 MHz，CoreMark 178.63"],
  ], { x: 8.15, y: 1.5, w: 4.63, h: 5.2, fontSize: 12 });
}

// P5 显存地址空间
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "DDR3 地址空间与帧缓冲布局");
  sectionTag(s, "02 · 硬件系统");
  figure(s, path.join(FIG, "rep_ddrmap.png"), 1.27, 1.4, 10.8, 4.57);
  caption(s, "DDR3 地址分区：程序区、双帧缓冲、表面表与素材区", 1.27, 6.02, 10.8);
  bullets(s, [
    ["帧格式", "XRGB8888 帧缓冲、ARGB8888 预乘纹理；行跨 4096 字节，一拍 128 位 = 4 像素"],
    ["双缓冲", "两块帧缓冲轮换呈现，切换仅发生于场消隐期"],
  ], { x: 1.27, y: 6.4, w: 10.8, h: 0.9, fontSize: 12 });
}

// P6 扫描输出与双缓冲
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "扫描输出：双缓冲防撕裂");
  sectionTag(s, "02 · 硬件系统");
  figure(s, path.join(FIG, "rep_scanout.png"), 0.92, 1.45, 11.5, 3.43);
  caption(s, "从 DDR3 帧缓冲到 HDMI 的扫描输出通路（640×480@60，TMDS）", 0.92, 4.92, 11.5);
  const cells = [
    ["场消隐锁存", "SWAP_REQ 只在消隐期生效，机制上排除撕裂"],
    ["可对账", "SWAP_COUNT 与场数对账，掉帧有客观定义"],
    ["实时性", "扫描 FIFO 512 拍；G4 最低 321 拍（含候选），欠载 0"],
  ];
  cells.forEach(([k, v], i) => {
    const x = 0.55 + i * 4.18;
    s.addShape("roundRect", { x, y: 5.45, w: 3.88, h: 1.3, rectRadius: 0.05,
      fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
    s.addText([
      { text: k, options: { bold: true, color: BLUE, breakLine: true } },
      { text: v, options: { color: INK } },
    ], { x: x + 0.2, y: 5.6, w: 3.5, h: 1.0, fontFace: FONT, fontSize: 11.5,
      valign: "top", margin: 0, isTextBox: true, lineSpacingMultiple: 1.15 });
  });
}

// P7 渲染引擎
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "GFX2D 渲染引擎：命令驱动的 BitBlt 加速器");
  sectionTag(s, "02 · 硬件系统");
  figure(s, path.join(FIG, "rep_engine.png"), 0.55, 1.32, 7.3, 4.77);
  caption(s, "渲染引擎内部分层结构", 0.55, 6.14, 7.3);
  bullets(s, [
    ["指令", "32 字节命令、五种操作码，表面表间接寻址"],
    ["总线", "AXI4 128 位主机，突发 ≤ 192 拍、读改写合并"],
    ["保护", "硬件裁剪、越界与重叠检查，十步错误模型"],
    ["预算", "实测事务模型驱动设计：行跨 4096 字节消解读方向固定开销"],
  ], { x: 8.15, y: 1.5, w: 4.63, h: 5.0, fontSize: 12 });
}

// P8 像素流水线
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "像素流水线：12 个 DSP 混合核");
  sectionTag(s, "02 · 硬件系统");
  figure(s, path.join(FIG, "rep_pixpipe.png"), 1.72, 1.35, 9.9, 4.76);
  caption(s, "像素流水线与混合方程（每周期 4 像素，预乘 Alpha 主路径）", 1.72, 6.16, 9.9);
  s.addText([
    { text: "吞吐　", options: { bold: true, color: BLUE, breakLine: false } },
    { text: "100 MHz 下理论 400 M 像素/秒；ALPHA 预乘路径每通道 1 次乘法，舍入式经 8,421,376 组全定义域穷举校验。",
      options: { color: INK } },
  ], { x: 1.72, y: 6.5, w: 9.9, h: 0.45, fontFace: FONT, fontSize: 12,
    valign: "middle", margin: 0, isTextBox: true });
}

// P9 软件分层与双目标
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "软件分层与双目标构建");
  sectionTag(s, "03 · 软件系统");
  figure(s, path.join(FIG, "rep_swlayers.png"), 0.55, 1.32, 7.5, 4.18);
  caption(s, "软件分层：应用 / 算法 lib / 板级支持 bsp / 厂商 sdk", 0.55, 5.56, 7.5);
  bullets(s, [
    ["强制分层", "可见范围由头文件搜索路径强制，越界引用直接编译失败"],
    ["双目标", "同一份代码编译为 PC（SDL2）与板上程序，board.h 两端语义一致"],
    ["分工", "CPU 只做游戏逻辑与命令表构造，逐像素运算全部交给引擎"],
    ["对账", "PC 上逐行对账、板上复现，通关录像 9208 帧逐行一致"],
  ], { x: 8.35, y: 1.5, w: 4.43, h: 5.0, fontSize: 12 });
}

// P10 素材流水线与启动
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "素材流水线与启动装载");
  sectionTag(s, "03 · 软件系统");
  figure(s, path.join(FIG, "rep_assets.png"), 1.59, 1.4, 10.15, 4.2);
  caption(s, "素材打包、Flash 镜像合成与上电装载", 1.59, 5.65, 10.15);
  bullets(s, [
    ["素材包", "20 项 2,840,320 字节，包 CRC b370ba0a；字库 Spleen（BSD-2）"],
    ["装载", "上电 bootloader 引导应用，SPI0 装入素材区并逐项校验，坏包九项判据显式拒绝"],
    ["自启动", "应用区写入后独立读回校验，断电重启 Flash 自启动验证通过"],
  ], { x: 1.59, y: 6.0, w: 10.15, h: 0.95, fontSize: 12 });
}

// P11 游戏画面：双端同源
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "同一份代码，两端运行");
  sectionTag(s, "03 · 软件系统");
  figure(s, path.join(HERE, "..", "Assert", "measurements", "pc_sw4_20261005",
    "pc_sw4_myriad_n101.png"), 0.55, 1.5, 5.9, 3.32);
  figure(s, path.join(HERE, "..", "Assert", "photos",
    "board_myriad_hw61_20261005_crop.jpg"), 6.88, 1.5, 5.9, 3.18);
  s.addText([
    { text: "PC 解释器（开发视图）", options: { bold: true, color: BLUE,
      breakLine: true } },
    { text: "SDL2 窗口，游戏逻辑与 HUD 统计最完整", options: { color: MUTE } },
  ], { x: 0.55, y: 4.95, w: 5.9, h: 0.75, fontFace: FONT, fontSize: 11.5,
    align: "center", margin: 0, isTextBox: true, lineSpacingMultiple: 1.2 });
  s.addText([
    { text: "板上硬件后端（屏摄）", options: { bold: true, color: BLUE,
      breakLine: true } },
    { text: "HDMI 输出，HUD 显示 HW FPS 61、活动弹 292", options: { color: MUTE } },
  ], { x: 6.88, y: 4.95, w: 5.9, h: 0.75, fontFace: FONT, fontSize: 11.5,
    align: "center", margin: 0, isTextBox: true, lineSpacingMultiple: 1.2 });
  s.addText([
    { text: "逐行对账　", options: { bold: true, color: BLUE, breakLine: false } },
    { text: "两端共享算法层与应用层代码，板上软件回放 600 帧与 PC 逐行一致；游戏 MYRIAD：四池实体管理、Q.8 定点数值、1280 项正弦表，终符兼作压力测试负载。",
      options: { color: INK } },
  ], { x: 0.55, y: 5.95, w: 12.23, h: 0.9, fontFace: FONT, fontSize: 12.5,
    valign: "middle", margin: 0, isTextBox: true, lineSpacingMultiple: 1.25 });
}

// P12 验证体系
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "验证体系：加速但不改画");
  sectionTag(s, "04 · 验证体系");
  figure(s, path.join(FIG, "rep_verify.png"), 1.57, 1.35, 10.2, 4.61);
  caption(s, "以同帧 CRC 为核心的四层验证体系", 1.57, 6.0, 10.2);
  bullets(s, [
    ["规格层", "舍入与位宽全定义域穷举；159 条语义向量带六项故障注入自检"],
    ["系统层", "十一帧金标准板上软硬各渲一帧，全帧逐像素 CRC 等价"],
  ], { x: 1.57, y: 6.33, w: 10.2, h: 0.62, fontSize: 12 });
}

// P13 帧率对比
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "实测：硬件加速 24 倍");
  sectionTag(s, "05 · 性能实测");
  figure(s, path.join(FIG, "rep_fps.png"), 0.55, 1.6, 8.6, 3.81);
  caption(s, "SW9 历史普通游戏与同 K 压力档帧率；S1 软件存量未稳态", 0.55, 5.5, 8.6);
  s.addShape("roundRect", { x: 9.5, y: 1.6, w: 3.28, h: 3.81, rectRadius: 0.06,
    fill: { color: DEEP } });
  s.addText([
    { text: "24×", options: { fontSize: 44, bold: true, color: WHITE,
      breakLine: true } },
    { text: "同游戏、同命令表", options: { fontSize: 11.5, color: "8FA8C4",
      breakLine: true } },
    { text: " ", options: { fontSize: 6, breakLine: true } },
    { text: "普通游戏：60 FPS", options: { fontSize: 12.5, bold: true,
      color: "E8EEF5", breakLine: true } },
    { text: "纯软件：2.5 FPS", options: { fontSize: 12.5, color: "C9DCEF",
      breakLine: true } },
    { text: " ", options: { fontSize: 6, breakLine: true } },
    { text: "S1 压力约 2030 颗：", options: { fontSize: 11.5, color: "8FA8C4",
      breakLine: true } },
    { text: "硬件 30.6 FPS", options: { fontSize: 12.5, bold: true,
      color: "E8EEF5", breakLine: true } },
    { text: "FIFO 零欠载", options: { fontSize: 11.5, color: "C9DCEF" } },
  ], { x: 9.78, y: 1.85, w: 2.72, h: 3.3, fontFace: FONT, valign: "top",
    margin: 0, isTextBox: true, lineSpacingMultiple: 1.2 });
  bullets(s, [
    ["口径", "掉帧 = 场数 − 呈现次数；帧率 = 呈现帧数 / 秒；确认档逐场零掉帧"],
    ["HUD", "屏上同屏显示软/硬两路帧率，一键切换后端——赛题「实时对比」的直接回应"],
  ], { x: 0.55, y: 5.95, w: 12.23, h: 1.0, fontSize: 12.5 });
}

// P14 N_max
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "实测：后端与缓存对照");
  sectionTag(s, "05 · 性能实测");
  figure(s, path.join(FIG, "rep_g4_nmax.png"), 1.72, 1.38, 9.9, 4.16);
  caption(s, "直通 / TEX 与条带使用同位流；两档条带缓存使用同一份程序",
    1.72, 5.6, 9.9);
  bullets(s, [
    ["判据", "确认档 3600 场且逐场零掉帧；按实际场数与呈现次数对账"],
    ["结果", "TEX：1891/2237；条带 4KB：424/505；16KB 候选：479/597；扫描欠载 0"],
    ["对照", "条带引擎正确执行，但 CPU 构造成本使整机吞吐下降；16KB 增益约 13%/18%"],
  ], { x: 1.72, y: 5.95, w: 9.9, h: 1.0, fontSize: 12 });
}

// P15 稳态
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "实测：稳态与余量");
  sectionTag(s, "05 · 性能实测");
  figure(s, path.join(FIG, "rep_stable.png"), 1.57, 1.45, 10.2, 4.53);
  caption(s, "P1 历史固定档 K896（约 1014 颗活动弹）连续 60 秒记录",
    1.57, 6.03, 10.2);
  bullets(s, [
    ["稳态", "P1 爬坡后约 16.3 ms，接近 16.67 ms 预算；帧周期与引擎忙周期分开报告"],
    ["一致性", "60 秒内仅首秒 59/60，其余 59 个窗口全部 60/60，无累积劣化"],
  ], { x: 1.57, y: 6.4, w: 10.2, h: 0.9, fontSize: 12 });
}

// P16 赛题要求逐条对照
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "赛题要求逐条对照");
  sectionTag(s, "06 · 对照与总结");
  const header = ["赛题要求", "本作品实现", "实测证据"].map(t => ({
    text: t, options: { bold: true, color: WHITE, fill: { color: BLUE } },
  }));
  const rows = [
    ["解析渲染指令", "32 字节命令，五种操作码，表面表间接寻址", "159 条语义向量全过；同帧 CRC 一致"],
    ["图像块拷贝与纯色填充", "BLIT / FILL，硬件裁剪与越界检查", "同帧 CRC 逐像素等价"],
    ["HDMI ≥ 640×480@60", "TMDS 640×480@60，上电彩条基线", "FRAME_COUNT 每秒 +60"],
    ["总线直访内存，突发与 FIFO", "AXI4 128 位主机，突发 ≤ 192 拍，多级 FIFO", "突发上限 256 拍实测；FIFO 欠载恒为 0"],
    ["C 驱动配置寄存器", "APB 寄存器组 + 命令表 + DOORBELL + 中断", "板上驱动全链路运行"],
    ["双缓冲防撕裂", "场消隐硬件锁存切换", "SWAP_COUNT 与场数对账；切换仅发生于场消隐期"],
    ["屏上实时对比软/硬帧率", "HUD 同屏两路帧率，一键切换后端", "60 FPS 对 2.5 FPS（约 24 倍）"],
    ["硬件 Alpha 混合（DSP）", "12 个 DSP 混合核，预乘主路径", "ALPHA 预乘 8,421,376 组穷举一致"],
    ["Color Keying", "按源格式整字色键，命中跳写", "KEY_SKIP 计数器对账"],
    ["高负载 Demo 冲精灵极限", "真实弹幕游戏 + 自动 N_max 搜索", "1891 / 2237 颗；3600 场零掉帧"],
  ].map(r => r.map(c => ({ text: c, options: { color: INK } })));
  s.addTable([header].concat(rows), {
    x: 0.55, y: 1.32, w: 12.23, colW: [3.2, 4.6, 4.43],
    fontFace: FONT, fontSize: 10.5, valign: "middle", align: "left",
    border: { type: "solid", color: LINE, pt: 0.75 },
    fill: { color: WHITE }, margin: 0.06,
    rowH: 0.46,
  });
}

// P17 资源与时序
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "资源与时序");
  sectionTag(s, "06 · 对照与总结");
  const header = ["资源", "平台基线", "整机（含引擎）", "占芯片总量"].map(t => ({
    text: t, options: { bold: true, color: WHITE, fill: { color: BLUE } },
  }));
  const rows = [
    ["XLR 逻辑单元", "16151", "41476", "68.2%"],
    ["片上 RAM（10 Kbit/块）", "65", "163", "63.7%"],
    ["DSP", "4", "16", "10.0%"],
    ["core_clk setup 余量", "+3.405 ns", "+0.756 ns", "无违例"],
  ].map(r => r.map(c => ({ text: c, options: { color: INK } })));
  s.addTable([header].concat(rows), {
    x: 0.55, y: 1.5, w: 7.3, colW: [2.5, 1.5, 1.8, 1.5],
    fontFace: FONT, fontSize: 12.5, valign: "middle", align: "left",
    border: { type: "solid", color: LINE, pt: 0.75 },
    fill: { color: WHITE }, margin: 0.07, rowH: 0.78,
  });
  bullets(s, [
    ["增量对应", "新增 12 个 DSP 与 12 个混合核一一对应"],
    ["新增 RAM", "TEX 16 块、BAND 36 块；16KB 缓存候选整机 181 块"],
    ["余量", "DSP 占用 10%，为画面扩展留出空间"],
    ["编译", "Efinity 2026.1 四步全过，无时序违例"],
  ], { x: 8.35, y: 1.7, w: 4.43, h: 4.5, fontSize: 12.5 });
}

// P18 总结
{
  const s = pres.addSlide({ masterName: "CONTENT" });
  pageTitle(s, "总结");
  sectionTag(s, "06 · 对照与总结");
  const inno = [
    ["预算先行", "以板上实测的 DDR3 事务模型反推架构：数据布局与访问形态消解读方向 3.8 倍固定开销，而非堆砌带宽"],
    ["加速不改画", "同一份逐像素规格驱动软硬两个实现，穷举校验 + 板上同帧 CRC，等价性可证明"],
    ["指标自动化", "N_max 定义为可执行判据：倍增到二分、候选档 3600 场确认，避免演示式定义争议"],
    ["健壮 ABI", "命令内建裁剪、越界、重叠检查与十步错误模型，驱动无需信任输入"],
  ];
  inno.forEach(([k, v], i) => {
    const x = 0.55 + (i % 2) * 6.27, y = 1.35 + Math.floor(i / 2) * 1.95;
    s.addShape("roundRect", { x, y, w: 5.96, h: 1.75, rectRadius: 0.05,
      fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
    s.addText([
      { text: k, options: { bold: true, color: BLUE, fontSize: 14,
        breakLine: true } },
      { text: v, options: { color: INK, fontSize: 11.5 } },
    ], { x: x + 0.25, y: y + 0.16, w: 5.46, h: 1.45, fontFace: FONT,
      valign: "top", margin: 0, isTextBox: true, lineSpacingMultiple: 1.18 });
  });
  s.addText([
    { text: "可扩展　", options: { bold: true, color: BLUE, breakLine: false } },
    { text: "混合方程表可扩展；提高分辨率需同步修改扫描时序、内存布局与条带参数并复测预算。条带已与软件对拍，下一步优化 CPU 构造，以同板 N_max 判断收益。",
      options: { color: INK } },
  ], { x: 0.55, y: 5.5, w: 12.23, h: 1.1, fontFace: FONT, fontSize: 12.5,
    valign: "middle", margin: 0, isTextBox: true, lineSpacingMultiple: 1.25 });
}

// P19 结尾
{
  const s = pres.addSlide({ masterName: "DARK" });
  s.addText("GFX2D", { x: 0.9, y: 2.6, w: 11.5, h: 1.1, fontFace: FONT,
    fontSize: 54, bold: true, color: WHITE, margin: 0, isTextBox: true });
  s.addText("把每一个周期都花在刀刃上，让每一个数字都能对账到一条日志",
    { x: 0.92, y: 3.85, w: 11.5, h: 0.5, fontFace: FONT, fontSize: 16,
      color: ICE, margin: 0, isTextBox: true });
  s.addText("谢谢评审指导", { x: 0.92, y: 4.7, w: 11.5, h: 0.4, fontFace: FONT,
    fontSize: 12.5, color: "8FA8C4", margin: 0, isTextBox: true });
}

pres.writeFile({ fileName: OUT }).then(() => {
  console.log("saved", OUT);
  return applyTheme(OUT, THEME);
}).then(() => console.log("theme applied")).catch(e => {
  console.error(e); process.exit(1);
});
