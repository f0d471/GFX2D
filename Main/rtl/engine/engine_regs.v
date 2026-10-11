`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_regs.v
//
// DESCRIPTION: 引擎寄存器层（APB 第 0 页）：ID/CAPS/CTRL/STATUS/错误定位/命令表
//   参数/表面表/PERF 计数与软复位握手。偏移与位号由 engine_regs_offsets.vh 生成，
//   本文件不手写数字。
//
// NOTE:
//   1. CTRL 的 SOFT_RST/CNT_CLR 写 1 自清，EN 挡 DOORBELL；忙/排空/EN=0 时写 DOORBELL 回 PSLVERR
//   2. 软复位举 soft_pending 直到 soft_rst_done，撤下拍清 SURF valid、错误定位与 DONE
//   3. 未映射偏移读写都回 PSLVERR；APB 主机在 ACCESS 结束沿采样，PSLVERR 同沿组合给出
//   4. PERF 事件按 I8 汇入，WAIT_DDR/BUSY 按电平每拍计一，引擎空闲自然不动
//==============================================================================================//
`default_nettype none
`include "engine_regs_offsets.vh"

module engine_regs #(parameter integer STRIP_H = 16) (
    input  wire        clk,
    input  wire        rst,            // 控制层复位（soc_system_reset），同步、高有效

    // APB 从口（B6 接第 0 页：psel 已按页门控，paddr 取页内 12 位）
    input  wire [11:0] paddr,
    input  wire        psel,
    input  wire        penable,
    input  wire        pwrite,
    input  wire [31:0] pwdata,
    output reg  [31:0] prdata,
    output wire        pready,
    output wire        pslverr,

    // 命令层（engine_cmd）状态与事件
    input  wire        cmd_busy,       // 已受理的命令表在跑（受理起、到收尾止）
    input  wire        err_set,        // 一拍：错误四元组同拍有效
    input  wire [3:0]  err_code,
    input  wire [31:0] err_index,
    input  wire [31:0] err_tag,
    input  wire [31:0] err_instance,
    input  wire [3:0]  err_stage,
    input  wire        perf_instance,
    input  wire        pb_busy,
    input  wire        perf_real_mode,
    input  wire        perf_axi_rd,
    input  wire        perf_axi_wr,
    input  wire        perf_pb_rd,
    input  wire        perf_pb_wr,
    input  wire        perf_bin,
    input  wire [1:0]  perf_refs,
    input  wire        done_set,       // 一拍：整表正常完成
    output wire        kick,           // 一拍：DOORBELL 受理（cmd 锁存 LIST_*）
    output wire [31:0] list_base,
    output wire [31:0] list_count,
    output wire        band_mode,
    output reg         bin_mode,
    output reg  [31:0] pb_base,
    output reg  [31:0] pb_bytes,
    output reg  [31:0] draw_cap,
    output reg  [31:0] ref_cap,
    output wire [31:0] lstrips,
    output wire [31:0] strip_log2,
    output wire [31:0] band_surfs,
    input  wire        soft_rst_done,  // 一拍：搬运层排空完成
    output wire        soft_rst_pending, // 电平：软复位请求举着（顶层 OR 进 drain_req）

    // 中断 B
    output wire        irq_b,          // 电平 = IRQ_STAT & IRQ_EN

    // PERF 事件（I8，顶层接线）
    input  wire        perf_cmd,       // 一拍：一条通过检查的命令提交（不含 END/错误）
    input  wire        perf_pixel_en,  // 一拍：一行边带握手，PIXEL += npx
    input  wire [10:0] perf_pixel_npx,
    input  wire        perf_key_en,    // 一拍：一拍写流，KEY_SKIP += popcount(lanes)
    input  wire [3:0]  perf_key_lanes,
    input  wire        perf_rdb,       // 一拍：一笔读突发提交（像素读或命令取数）
    input  wire [11:0] perf_rdb_beats,
    input  wire        perf_wrb,       // 一拍：一笔写突发提交
    input  wire [11:0] perf_wrb_beats,
    input  wire        perf_wait,      // 电平：AXI 一侧有 ready 未握手的等待拍
    input  wire        perf_busy,      // 电平：引擎忙

    // 表面表读口（engine_cmd 的十步检查用，组合读）
    input  wire [3:0]  surf_ra_id,
    input  wire [3:0]  surf_rb_id,
    output wire [31:0] surf_ra_w0,
    output wire [31:0] surf_ra_w1,
    output wire [31:0] surf_ra_w2,
    output wire [31:0] surf_rb_w0,
    output wire [31:0] surf_rb_w1,
    output wire [31:0] surf_rb_w2
);

    // CAPS 按当前 profile 输出（编码见 engine_regs_offsets.vh）
    wire [31:0] caps_value = EC_CAPS_VALUE;

    localparam [11:0] SURF_LAST = EC_REG_SURF0 + 12'd192;   // 16 槽 × 12 字节
    localparam [11:0] PERF_LAST = EC_REG_PERF0 + 12'd4 * EC_PERF_COUNT;

    // 寄存器
    reg         ctrl_en;
    reg         ctrl_band;
    reg [31:0]  lstrips_r, strip_log2_r, band_surfs_r;
    reg         soft_pending;
    reg [31:0]  list_base_r;
    reg [31:0]  list_count_r;
    reg         st_done, st_err;
    reg [3:0]   err_code_r;
    reg [31:0]  err_index_r, err_tag_r, err_instance_r;
    reg [3:0]   err_stage_r;
    reg [1:0]   irq_en_r;
    reg [1:0]   irq_stat;

    assign band_mode = ctrl_band;
    assign lstrips = lstrips_r;
    assign strip_log2 = strip_log2_r;
    assign band_surfs = band_surfs_r;
    assign list_base = list_base_r;
    assign list_count = list_count_r;
    assign soft_rst_pending = soft_pending;

    wire [31:0] status_value = {20'd0, err_code_r, st_err, st_done, soft_pending, cmd_busy};
    assign irq_b = (irq_stat[EC_IRQ_LIST_DONE_BIT] & irq_en_r[EC_IRQ_LIST_DONE_BIT])
                 | (irq_stat[EC_IRQ_ERROR_BIT]    & irq_en_r[EC_IRQ_ERROR_BIT]);

    // APB 译码
    wire access    = psel && penable;
    wire wr_access = access && pwrite;
    wire rd_setup  = psel && !penable && !pwrite;

    wire wr_ctrl = wr_access && (paddr == EC_REG_CTRL);
    wire wr_door = wr_access && (paddr == EC_REG_DOORBELL);
    wire door_accepted = wr_door && ctrl_en && !cmd_busy && !soft_pending;

    wire surf_addr = (paddr >= EC_REG_SURF0) && (paddr < SURF_LAST)
                  && (paddr[1:0] == 2'b00);
    wire perf_addr = (paddr >= EC_REG_PERF0) && (paddr < PERF_LAST)
                  && (paddr[1:0] == 2'b00);
    wire rd_mapped = (paddr == EC_REG_ID)    || (paddr == EC_REG_CAPS)
                   || (paddr == EC_REG_CTRL)  || (paddr == EC_REG_STATUS)
                   || (paddr == EC_REG_IRQ_EN)|| (paddr == EC_REG_IRQ_STAT)
                   || (paddr == EC_REG_ERROR_INDEX) || (paddr == EC_REG_ERROR_TAG)
                   || (paddr == EC_REG_ERROR_INSTANCE)
                   || (paddr == EC_REG_LIST_BASE)   || (paddr == EC_REG_LIST_COUNT)
                   || (paddr == EC_REG_LSTRIPS) || (paddr == EC_REG_STRIP_H)
                   || (paddr == EC_REG_BAND_SURF) || surf_addr || perf_addr;
    wire pb_addr = paddr == EC_REG_PB_BASE || paddr == EC_REG_PB_BYTES ||
                   paddr == EC_REG_DRAW_CAP || paddr == EC_REG_REF_CAP;
    wire wr_mapped = (paddr == EC_REG_CTRL) || (paddr == EC_REG_IRQ_EN)
                   || (paddr == EC_REG_IRQ_STAT) || (paddr == EC_REG_LIST_BASE)
                   || (paddr == EC_REG_LIST_COUNT) || (paddr == EC_REG_DOORBELL)
                   || (paddr == EC_REG_LSTRIPS) || (paddr == EC_REG_STRIP_H)
                   || (paddr == EC_REG_BAND_SURF) || pb_addr || surf_addr;
    wire pb_write_block = (pb_addr && cmd_busy) || (surf_addr && (pb_busy === 1'b1));
    // APB 主机在 ACCESS 的结束边沿采样 PSLVERR；同一边沿才置寄存器会晚一拍。
    assign pslverr = access && (pwrite ? (!wr_mapped || pb_write_block || (wr_door && !door_accepted))
                                       : !(rd_mapped || pb_addr || paddr == EC_REG_ERROR_STAGE));

    // SURF 译码：每项 3 字、步长 12 字节。12 不是 2 的幂，
    // 用 16 档的「够不够 3 的整数倍」比较链译码（写读共用，只挂 APB 访问）。
    wire       surf_wr_sel = wr_access && surf_addr;
    wire [7:0] surf_off    = paddr[7:0] - 8'h40;      // 窗口内字节偏移 [0,192)
    wire [5:0] surf_widx   = surf_off[7:2];           // 窗口内字号 [0,47]
    reg  [3:0] surf_slot_id;
    reg  [1:0] surf_word_sel;
    reg  [6:0] surf_diff;
    integer    surf_i;
    always @* begin
        // 向上循环、最后一次赋值生效：命中的是满足 widx >= slot*3 的最大 slot。
        // 两项各补一位零扩展到 7 位相减，数值不变且没有截断；
        // 不用 7'(...) 尺寸转换：iverilog 接受，Efinity 综合报语法错。
        for (surf_i = 0; surf_i <= 15; surf_i = surf_i + 1) begin
            surf_diff = {1'b0, surf_widx} - {1'b0, surf_i[5:0] * 6'd3};
            if (!surf_diff[6] && (surf_diff >= 0)) begin
                surf_slot_id  = surf_i[3:0];
                surf_word_sel = surf_diff[1:0];
            end
        end
    end

    // SURF 存储
    reg [31:0] surf_w0 [0:15];
    reg [31:0] surf_w1 [0:15];
    reg [31:0] surf_w2w [0:15];

    assign surf_ra_w0 = surf_w0[surf_ra_id];
    assign surf_ra_w1 = surf_w1[surf_ra_id];
    assign surf_ra_w2 = surf_w2w[surf_ra_id];
    assign surf_rb_w0 = surf_w0[surf_rb_id];
    assign surf_rb_w1 = surf_w1[surf_rb_id];
    assign surf_rb_w2 = surf_w2w[surf_rb_id];

    // SURF 读数据（APB 读用，同一套译码）
    reg [31:0] surf_rd_word;
    always @* begin
        case (surf_word_sel)
            2'd0:    surf_rd_word = surf_w0[surf_slot_id];
            2'd1:    surf_rd_word = surf_w1[surf_slot_id];
            default: surf_rd_word = surf_w2w[surf_slot_id];
        endcase
    end

    // PERF 计数器
    reg [31:0] perf [0:EC_PERF_COUNT-1];
    wire       cnt_clr = wr_ctrl && pwdata[EC_CTRL_CNT_CLR_BIT];

    wire [3:0] key_lanes = perf_key_lanes;
    function [31:0] pc4(input [3:0] v);
        pc4 = {29'd0, v[0]} + {29'd0, v[1]} + {29'd0, v[2]} + {29'd0, v[3]};
    endfunction

    // APB 读数据
    reg [31:0] rdata_mux;
    // PERF 窗口 64 字节对齐：paddr[5:2] 就是窗内计数器号（rd_mapped 已限 < PERF_LAST）
    wire [31:0] perf_rd = perf[paddr[5:2]];
    always @* begin
        case (paddr)
            EC_REG_ID:            rdata_mux = EC_ID_VALUE;
            EC_REG_CAPS:          rdata_mux = caps_value;
            EC_REG_CTRL:          rdata_mux = {27'd0, bin_mode, ctrl_band, 2'd0, ctrl_en};
            EC_REG_STATUS:        rdata_mux = status_value;
            EC_REG_IRQ_EN:        rdata_mux = {30'd0, irq_en_r};
            EC_REG_IRQ_STAT:      rdata_mux = {30'd0, irq_stat};
            EC_REG_ERROR_INDEX:   rdata_mux = err_index_r;
            EC_REG_ERROR_TAG:     rdata_mux = err_tag_r;
            EC_REG_ERROR_INSTANCE: rdata_mux = err_instance_r;
            EC_REG_ERROR_STAGE:   rdata_mux = {28'd0, err_stage_r};
            EC_REG_PB_BASE:       rdata_mux = pb_base;
            EC_REG_PB_BYTES:      rdata_mux = pb_bytes;
            EC_REG_DRAW_CAP:      rdata_mux = draw_cap;
            EC_REG_REF_CAP:       rdata_mux = ref_cap;
            EC_REG_LIST_BASE:     rdata_mux = list_base_r;
            EC_REG_LIST_COUNT:    rdata_mux = list_count_r;
            EC_REG_LSTRIPS:       rdata_mux = lstrips_r;
            EC_REG_STRIP_H:       rdata_mux = strip_log2_r;
            EC_REG_BAND_SURF:     rdata_mux = band_surfs_r;
            default:              rdata_mux = (paddr >= EC_REG_PERF0) ? perf_rd : surf_rd_word;
        endcase
    end

    // 主时序
    integer i;
    always @(posedge clk) begin
        if (rst) begin
            ctrl_en <= 1'b0;
            ctrl_band <= 1'b0;
            bin_mode <= 0;
            pb_base <= 0;
            pb_bytes <= 0;
            draw_cap <= 1024;
            ref_cap <= 8192;
            err_stage_r <= 0;
            lstrips_r <= 480/STRIP_H;
            strip_log2_r <= $clog2(STRIP_H);
            band_surfs_r <= 0;
            soft_pending <= 1'b0;
            list_base_r <= 32'd0;
            list_count_r <= 32'd0;
            st_done <= 1'b0;
            st_err <= 1'b0;
            err_code_r <= 4'd0;
            err_index_r <= 32'd0;
            err_tag_r <= 32'd0;
            err_instance_r <= 32'hffffffff;
            irq_en_r <= 2'd0;
            irq_stat <= 2'd0;
            prdata <= 32'd0;
            for (i = 0; i < 16; i = i + 1) begin
                surf_w0[i] <= 32'd0;
                surf_w1[i] <= 32'd0;
                surf_w2w[i] <= 32'd0;
            end
            for (i = 0; i < EC_PERF_COUNT; i = i + 1) perf[i] <= 32'd0;
        end else begin
            // 读：SETUP 相锁存，ACCESS 相给出（pready 恒 1，零等待）
            if (rd_setup) begin
                prdata <= (rd_mapped || pb_addr || paddr == EC_REG_ERROR_STAGE) ? rdata_mux : 32'd0;
            end

            // 写
            if (wr_ctrl) begin
                ctrl_en <= pwdata[EC_CTRL_EN_BIT];
                ctrl_band <= pwdata[EC_CTRL_BAND_BIT];
                if (!cmd_busy) bin_mode <= pwdata[EC_CTRL_BIN_BIT];
                if (cnt_clr)
                    for (i = 0; i < EC_PERF_COUNT; i = i + 1) perf[i] <= 32'd0;
                if (pwdata[EC_CTRL_SOFT_RST_BIT] && !soft_pending)
                    soft_pending <= 1'b1;
            end else if (wr_access && paddr == EC_REG_IRQ_EN) begin
                irq_en_r <= pwdata[1:0];
            end else if (wr_access && paddr == EC_REG_IRQ_STAT) begin
                irq_stat <= irq_stat & ~pwdata[1:0];
            end else if (wr_access && paddr == EC_REG_LIST_BASE) begin
                list_base_r <= pwdata;
            end else if (wr_access && paddr == EC_REG_LIST_COUNT) begin
                list_count_r <= pwdata;
            end else if (wr_access && paddr == EC_REG_LSTRIPS) begin
                lstrips_r <= pwdata;
            end else if (wr_access && paddr == EC_REG_STRIP_H) begin
                strip_log2_r <= pwdata;
            end else if (wr_access && paddr == EC_REG_BAND_SURF) begin
                band_surfs_r <= pwdata;
            end else if (wr_access && pb_addr && !pb_write_block) begin
                case (paddr)
                    EC_REG_PB_BASE: pb_base <= pwdata;
                    EC_REG_PB_BYTES: pb_bytes <= pwdata;
                    EC_REG_DRAW_CAP: draw_cap <= pwdata;
                    EC_REG_REF_CAP: ref_cap <= pwdata;
                endcase
            end else if (surf_wr_sel && !pb_write_block) begin
                case (surf_word_sel)
                    2'd0:    surf_w0[surf_slot_id] <= pwdata;
                    2'd1:    surf_w1[surf_slot_id] <= pwdata;
                    default: surf_w2w[surf_slot_id] <= pwdata;
                endcase
            end
            // DOORBELL 的拒绝由 ACCESS 相组合 PSLVERR 返回；受理本身不写寄存器。

            // 软复位排空完成：撤请求、清现场（valid、错误定位、DONE）
            if (soft_pending && soft_rst_done) begin
                soft_pending <= 1'b0;
                st_done <= 1'b0;
                st_err <= 1'b0;
                err_code_r <= 4'd0;
                err_stage_r <= 0;
                err_index_r <= 32'd0;
                err_tag_r <= 32'd0;
                err_instance_r <= 32'hffffffff;
                for (i = 0; i < 16; i = i + 1)
                    if (surf_w2w[i][31])
                        surf_w2w[i] <= surf_w2w[i] & 32'h7FFF_FFFF;
            end

            // DOORBELL 受理：清旧错误定位与 DONE
            if (door_accepted) begin
                st_done <= 1'b0;
                st_err <= 1'b0;
                err_code_r <= 4'd0;
                err_index_r <= 32'd0;
                err_tag_r <= 32'd0;
                err_instance_r <= 32'hffffffff;
                err_stage_r <= 0;
            end

            // 命令层事件
            if (err_set) begin
                st_err <= 1'b1;
                err_code_r <= err_code;
                err_index_r <= err_index;
                err_tag_r <= err_tag;
                err_instance_r <= err_instance;
                err_stage_r <= err_stage;
                irq_stat[EC_IRQ_ERROR_BIT] <= 1'b1;
            end
            if (done_set) begin
                st_done <= 1'b1;
                irq_stat[EC_IRQ_LIST_DONE_BIT] <= 1'b1;
            end

            // PERF 计数：引擎空闲时 PIXEL/WAIT/BUSY 自然不动
            if (perf_instance === 1'b1) perf[EC_PERF_INSTANCE] <= perf[EC_PERF_INSTANCE] + 32'd1;
            if (perf_refs != 0) perf[EC_PERF_REF_COUNT] <= perf[EC_PERF_REF_COUNT] + {30'd0, perf_refs};
            if (perf_bin === 1'b1) perf[EC_PERF_BIN_CYCLES] <= perf[EC_PERF_BIN_CYCLES] + 1;
            if (perf_pb_rd === 1'b1) perf[EC_PERF_PB_RD_BEAT] <= perf[EC_PERF_PB_RD_BEAT] + 1;
            if (perf_pb_wr === 1'b1) perf[EC_PERF_PB_WR_BEAT] <= perf[EC_PERF_PB_WR_BEAT] + 1;
            if (perf_real_mode === 1'b1) begin
                if (perf_axi_rd) perf[EC_PERF_RD_BEAT] <= perf[EC_PERF_RD_BEAT] + 1;
                if (perf_axi_wr) perf[EC_PERF_WR_BEAT] <= perf[EC_PERF_WR_BEAT] + 1;
            end
            if (perf_cmd) perf[EC_PERF_CMD] <= perf[EC_PERF_CMD] + 32'd1;
            if (perf_pixel_en) perf[EC_PERF_PIXEL] <= perf[EC_PERF_PIXEL] + {21'd0, perf_pixel_npx};
            if (perf_key_en) perf[EC_PERF_KEY_SKIP] <= perf[EC_PERF_KEY_SKIP] + pc4(key_lanes);
            if (perf_rdb) begin
                perf[EC_PERF_RD_BURST] <= perf[EC_PERF_RD_BURST] + 32'd1;
                if (!(perf_real_mode === 1'b1)) perf[EC_PERF_RD_BEAT] <= perf[EC_PERF_RD_BEAT] + {20'd0, perf_rdb_beats};
            end
            if (perf_wrb) begin
                perf[EC_PERF_WR_BURST] <= perf[EC_PERF_WR_BURST] + 32'd1;
                if (!(perf_real_mode === 1'b1)) perf[EC_PERF_WR_BEAT] <= perf[EC_PERF_WR_BEAT] + {20'd0, perf_wrb_beats};
            end
            if (perf_wait) perf[EC_PERF_WAIT_DDR] <= perf[EC_PERF_WAIT_DDR] + 32'd1;
            if (perf_busy) perf[EC_PERF_BUSY] <= perf[EC_PERF_BUSY] + 32'd1;
        end
    end

    assign pready = 1'b1;
    assign kick   = door_accepted;

endmodule
`default_nettype wire
