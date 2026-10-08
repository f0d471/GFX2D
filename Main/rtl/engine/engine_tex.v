// engine_tex：v1-TEX profile 的片上纹理（引擎 P3 G1，定案 D57）
//
// 16 KiB scratchpad：四个 1024×32 bank。APB 写口每次写一个 32 位字（字地址低两位
// 选 bank）；读流与 APB 读回复用 128 位宽读口（同行号读四个 bank 拼拍）。
// 不做 cache 标签、不做替换、不做 BLIT-to-TEX；RAM 内容不做全量复位，
// 控制复位只清执行现场（在途描述符与流水），内容保留。
//
// APB（第 4~7 页，psel 已按 paddr[15:14]==01 门控，本模块收窗口内 14 位字节地址）：
//   * 只允许 4 字节对齐的 32 位访问，未对齐回 PSLVERR 且不得改写 RAM；
//   * 读在 SETUP 相发同步读请求、ACCESS 相给出数据（pready 恒 1，零等待）；
//   * 写在 ACCESS 相（完成沿）提交；
//   * 宽读口给读流优先：两者同拍竞争时 APB 读拿不到口，读回值不保证——
//     运行期 CPU 不得读写 TEX 是驱动纪律（总体设计 3.4 节 2026-10-07 定案段），
//     硬件不加互锁。
//
// 读流（I3）：描述符口收「字节地址 + 字节数」，每拍读一个 128 位字，经
// 同步 RAM 的一级输出（READ_LATENCY=1）或再加 q1（=2）给出 data/valid/tlast——valid 允许
// 有空拍（描述符受理与收尾各一拍），一个描述符之内 valid 连续；消费者恒 ready
//（D23/I3 契约），所以本模块没有 ready 口、不做 skid buffer。上下文（缓冲号、
// 命令索引）在描述符口锁存，随流（rs_buf）与状态（stat_*）带回。
// 状态在末拍之后有效，直到 stat_ready 握手；上下文保持且 busy 不撤。
//
// 描述符恰好一笔的纪律由几何层保证（跨 4KB 已拆段）；本模块把地址截到窗口内
// 使用，越界描述符是上游检查（engine_cmd 的 TEX 结构两条）的责任。
//
// 注错（变异测试）：本文件是干净形态，三处变异各包在 `ifdef 内。
// 都不定义时它们不进编译输入（预处理阶段已删除），不依赖综合器优化。
// 编译开关与 sim/engine/tex/run.sh 的用例一一对应，一次只开一个：
//   INJ_VALID_EARLY  valid 提前一级：valid 与 data 错拍（研究变异的正式版，
//                    全路径内存比对必须见红）
//   INJ_WR_LOOSE     未对齐 APB 写放行（未对齐拒绝用例必须见红）
//   INJ_STAT_DROP    状态脉冲吞掉（所有走 TEX 的用例挂看门狗，必须见红）
//
// 设计依据：引擎 P3 总览 D57、engine 第 07 章（前期研究与定义修正）；
// RAM 组织与块数（四 bank 16 块）的实验证据在同章 3.5 节。
`timescale 1ns/1ps
`default_nettype none

module engine_tex #(
    parameter integer READ_LATENCY = 1
)(
    input  wire        clk,
    input  wire        rst,           // 控制层复位（soc_system_reset），同步

    // ---- APB 从口（窗口内字节地址，psel 已按页门控）----
    input  wire [13:0] paddr,
    input  wire        psel,
    input  wire        penable,
    input  wire        pwrite,
    input  wire [31:0] pwdata,
    output wire [31:0] prdata,
    output wire        pready,
    output wire        pslverr,

    // ---- 读描述符口（搬运层 → 本模块）----
    input  wire [13:0] req_addr,      // 字节地址，16 字节对齐
    input  wire [11:0] req_bytes,     // 字节数，16 的倍数
    input  wire [1:0]  req_buf,       // 目标行缓冲号（随流带回）
    input  wire [3:0]  req_cidx,      // 命令索引（随状态带回）
    input  wire        req_valid,
    output wire        req_ready,

    // ---- 读流（本模块 → 搬运层；消费者恒 ready，无 ready 口）----
    output wire [127:0] rs_tdata,
    output wire         rs_tvalid,
    output wire         rs_tlast,
    output wire [1:0]   rs_buf,

    // ---- 状态与占用 ----
    output wire         stat_valid,   // 末拍之后保持，直到 stat_ready 采样
    input  wire         stat_ready,
    output wire [3:0]   stat_cidx,
    output wire [1:0]   stat_buf,
    output wire         busy          // 描述符在途（受理起到状态握手止）
);

    // =====================================================================
    // APB 译码
    // =====================================================================
    wire       apb_access   = psel && penable;
    wire       apb_wr_acc   = apb_access && pwrite;
    wire       apb_rd_setup = psel && !penable && !pwrite;
    wire       apb_aligned  = (paddr[1:0] == 2'b00);
`ifdef INJ_WR_LOOSE
    wire       apb_wr_en    = apb_wr_acc;              // 注错：未对齐写也放行
`else
    wire       apb_wr_en    = apb_wr_acc && apb_aligned;
`endif
    wire       apb_rd_req   = apb_rd_setup && apb_aligned;
    wire [9:0] apb_row      = paddr[13:4];
    wire [1:0] apb_wsel     = paddr[3:2];

    // 未对齐回 PSLVERR；对齐访问全窗口映射，没有第二个错误来源
    assign pslverr = apb_access && !apb_aligned;
    assign pready  = 1'b1;

    // =====================================================================
    // 读流引擎
    // =====================================================================
    reg         busy_r;
    reg  [11:0] left;          // 剩余拍数
    reg  [9:0]  ra_cur;        // 当前宽读行号（16KiB = 1024 行）
    reg  [1:0]  ctx_buf;
    reg  [3:0]  ctx_cidx;
    reg  [127:0] q1;
    reg         v0, l0;        // RAM 输出 rq 对应 v0/l0；可选 q1 对应 v1/l1
    reg         v1, l1;
    reg         stat_r;

    wire        str_fire = req_valid && req_ready;
    wire        issue    = busy_r && (left != 12'd0) && !stat_r;

    // 宽读口：读流优先（见文件头）；APB 读只在流不占口的那一拍真正发生
    wire        rd_port_re = issue || (apb_rd_req && !busy_r);
    wire [9:0]  rd_port_ra = issue ? ra_cur : apb_row;

    assign req_ready = !busy_r;
    assign busy      = busy_r;

    // 一级同步读，q1/v1/l1 为可选的第二级；注错让 valid/last 提前一级。
    always @(posedge clk) begin
        if (rst) begin
            busy_r   <= 1'b0;
            left     <= 12'd0;
            ra_cur   <= 10'd0;
            ctx_buf  <= 2'd0;
            ctx_cidx <= 4'd0;
            q1       <= 128'd0; v0 <= 1'b0; l0 <= 1'b0;
            v1 <= 1'b0; l1 <= 1'b0;
            stat_r   <= 1'b0;
        end else begin
            // 受理：锁存上下文，下一拍开始发宽读
            if (str_fire) begin
                busy_r   <= 1'b1;
                left     <= req_bytes >> 4;
                ra_cur   <= req_addr[13:4];
                ctx_buf  <= req_buf;
                ctx_cidx <= req_cidx;
            end else if (stat_r && stat_ready) begin
                busy_r <= 1'b0;
            end

            if (issue) begin
                ra_cur <= ra_cur + 10'd1;
                left   <= left - 12'd1;
            end

            v0 <= issue;
            l0 <= issue && (left == 12'd1);
            q1 <= bank_q;

            v1 <= v0;
            l1 <= l0;

            if (stat_r && stat_ready) stat_r <= 1'b0;
`ifdef INJ_STAT_DROP
            // 注错：状态脉冲吞掉
`else
            if (rs_tvalid && rs_tlast) stat_r <= 1'b1;
`endif
        end
    end

    // RAM 自带同步输出级；可选 q1 只用于二拍配置。
    assign rs_tdata  = READ_LATENCY == 1 ? bank_q : q1;
`ifdef INJ_VALID_EARLY
    // 注错：valid/last 提前一级，与 data 错拍
    assign rs_tvalid = (READ_LATENCY == 1) ? issue : v0;
    assign rs_tlast  = (READ_LATENCY == 1) ? (issue && left == 1) : l0;
`else
    assign rs_tvalid = (READ_LATENCY == 1) ? v0 : v1;
    assign rs_tlast  = (READ_LATENCY == 1) ? l0 : l1;
`endif
    assign rs_buf    = ctx_buf;
    assign stat_valid = stat_r;
    assign stat_cidx  = ctx_cidx;
    assign stat_buf   = ctx_buf;

    // =====================================================================
    // 四个 1024×32 bank（研究探针 probe_tex_ram.v 的组织，实测 16 块 RAM）
    // =====================================================================
    wire [127:0] bank_q;
    genvar b;
    generate
        for (b = 0; b < 4; b = b + 1) begin : g_bank
            reg [31:0] mem [0:1023];
            reg [31:0] rq;
            always @(posedge clk) begin
                if (apb_wr_en && (paddr[3:2] == b[1:0]))
                    mem[apb_row] <= pwdata;
                if (rd_port_re)
                    rq <= mem[rd_port_ra];
            end
            assign bank_q[32*b +: 32] = rq;
        end
    endgenerate

    // APB 读：SETUP 相发宽读（rd_port_re），ACCESS 相按 SETUP 锁存的字选给数据。
    // 读流优先的取舍之下，只有驱动守纪律（引擎空闲期访问）时读回值才有定义。
    reg [1:0] prd_wsel;
    always @(posedge clk) begin
        if (rst)                  prd_wsel <= 2'd0;
        else if (apb_rd_setup)    prd_wsel <= apb_wsel;
    end
    assign prdata = apb_access ? bank_q[32*prd_wsel +: 32] : 32'd0;

endmodule
`default_nettype wire
