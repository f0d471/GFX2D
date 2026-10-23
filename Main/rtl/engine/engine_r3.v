`timescale 1ns/1ps
// R3 参数影子/门铃快照、三维数据通路与事务收尾。
// 不调度帧：go/clear/phase/ack 只由 engine_cmd 产生。mover drain 会吞 FS，
// 所以取消后的读者只能在物理 drain_done 之后局部复位；像素/TEX 返回先正常退休。
`default_nettype none
`include "engine_feature.vh"
`include "engine_regs_offsets.vh"
module engine_r3 #(parameter integer TEX_LATENCY=1)(
    input wire clk, rst, kick, finish, soft_reset, abort_frame,
    input wire band_mode,
    input wire [31:0] lstrips,
    input wire [11:0] paddr,
    input wire psel, penable, pwrite,
    input wire [31:0] pwdata,
    output reg [31:0] prdata,
    output wire pready, pslverr,
    output wire enable, cfg_bad,
    output reg selected,
    input wire go, clear, phase, strip_ack,
    output reg clear_done,
    output wire strip_req,
    output wire [4:0] cur_strip,
    output wire strip_done,
    output reg frame_done,
    output wire idle, quiet, drain_req,
    input wire mover_done, timeout_event,
    output reg error,
    output reg [3:0] error_code,
    output reg [31:0] error_index, error_tag,
    output wire [31:0] fd_addr,
    output wire [11:0] fd_bytes,
    output wire [3:0] fd_cidx,
    output wire fd_valid,
    input wire fd_ready,
    input wire [127:0] fs_data,
    input wire fs_valid, fs_last,
    input wire [15:0] fs_keep,
    input wire [3:0] fs_cidx,
    output wire fs_ready,
    input wire stat_valid, stat_error,
    input wire [3:0] stat_cidx,
    output wire ras_lease,
    output wire [13:0] ras_pix,
    output wire [31:0] ras_data,
    output wire ras_valid,
    input wire ras_ready,
    output wire samp_valid,
    input wire samp_ready,
    output wire [11:0] samp_word,
    input wire samp_rvalid,
    input wire [31:0] samp_rdata
);
    reg [31:0] sh_tri, sh_sub, sh_bytes, sh_count, sh_key, sh_tex;
    reg [31:0] tri_base_r, sub_base_r, bytes_r, count_r, key_r;
    wire [32:0] envelope = ({1'b0,sh_bytes}+33'd15) & 33'h1fffffff0;
    assign enable = sh_count != 0;
    assign cfg_bad = !band_mode || lstrips!=32'd30 || sh_count>32'd65535 ||
        sh_tri[4:0]!=0 || sh_sub[3:0]!=0 || sh_bytes[31:24]!=0 ||
        sh_bytes[0] || sh_bytes<32'd120 || sh_tex!=0 ||
        ({1'b0,sh_tri}+({1'b0,sh_count}<<7))>33'h100000000 ||
        ({1'b0,sh_sub}+envelope)>33'h100000000;
    reg mapped, writable;
    reg [31:0] read_value;
    reg [31:0] cycles;
    // 每帧最多30*65535=1966050条引用，21位够用；低位模差也覆盖全局debug计数回绕。
    reg [20:0] retire_base, retired_frame;
    wire [31:0] retired;
    wire rlst_busy, core_idle, z_busy, z_done, list_frame_done, list_strip_req;
    reg req_seen;
    assign strip_req=req_seen;
    wire list_error, core_error;
    wire [3:0] list_code, core_code;
    wire [15:0] list_index, core_index;
    wire [15:0] list_active_index;
    wire [31:0] list_active_tag;
    wire [31:0] list_tag, core_tag;
    reg started, mover_seen;
    reg [2:0] samples;
    wire first_event = started && (timeout_event || list_error || core_error);
    wire stopping = rst || (selected && (soft_reset || abort_frame || error || first_event));
    assign drain_req = stopping;
    assign idle = !rlst_busy && core_idle && !z_busy && samples==0;
    assign quiet = mover_seen && idle;
    assign ras_lease = phase && selected && !stopping;
    always @* begin
        mapped=1'b1; writable=1'b0; read_value=0;
        case(paddr)
            EC_R3_ID: read_value=EC_R3_ID_VALUE;
            EC_R3_CAPS: read_value=EC_CAPS_VALUE & 32'h0000c000;
            EC_R3_TRI_BASE: begin read_value=sh_tri; writable=1;end
            EC_R3_SUB_BASE: begin read_value=sh_sub; writable=1;end
            EC_R3_SUB_BYTES: begin read_value=sh_bytes; writable=1;end
            EC_R3_TRI_COUNT: begin read_value=sh_count; writable=1;end
            EC_R3_RAS_KEY: begin read_value=sh_key; writable=1;end
            EC_R3_TEXCFG: begin read_value=sh_tex; writable=1;end
            EC_R3_PERF_CYCLES: read_value=cycles;
            EC_R3_PERF_RETIRED: read_value={11'd0,retired_frame};
            EC_R3_DEBUG: read_value={19'd0,samples,3'd0,z_busy,core_idle,rlst_busy,
                                      frame_done,error,started,selected};
            default: mapped=0;
        endcase
    end
    assign pready=1;
    assign pslverr=psel && penable && (!mapped || (pwrite && !writable));
    always @(posedge clk) begin
        if(rst) begin
            sh_tri<=0;sh_sub<=0;sh_bytes<=0;sh_count<=0;sh_key<=0;sh_tex<=0;
            tri_base_r<=0;sub_base_r<=0;bytes_r<=0;count_r<=0;key_r<=0;prdata<=0;
            selected<=0;started<=0;mover_seen<=0;clear_done<=0;frame_done<=0;
            error<=0;error_code<=0;error_index<=0;error_tag<=0;
            cycles<=0;retire_base<=0;retired_frame<=0;samples<=0;req_seen<=0;
        end else begin
            if(psel && !penable && !pwrite) prdata<=mapped ? read_value:32'd0;
            if(psel && penable && pwrite && writable) case(paddr)
                EC_R3_TRI_BASE:sh_tri<=pwdata;
                EC_R3_SUB_BASE:sh_sub<=pwdata;
                EC_R3_SUB_BYTES:sh_bytes<=pwdata;
                EC_R3_TRI_COUNT:sh_count<=pwdata;
                EC_R3_RAS_KEY:sh_key<=pwdata;
                EC_R3_TEXCFG:sh_tex<=pwdata;
                default:begin end
            endcase
            if(kick) begin
                tri_base_r<=sh_tri;sub_base_r<=sh_sub;bytes_r<=sh_bytes;count_r<=sh_count;key_r<=sh_key;
                selected<=enable;started<=0;mover_seen<=0;frame_done<=0;clear_done<=0;
                error<=0;error_code<=0;error_index<=0;error_tag<=0;cycles<=0;
                retire_base<=retired[20:0];retired_frame<=0;req_seen<=0;
            end else begin
                if(go) started<=1;
                if(list_strip_req) req_seen<=1;
                else if(strip_ack) req_seen<=0;
                if(selected && phase) cycles<=cycles+1'b1;
                if(selected && !quiet) retired_frame<=retired[20:0]-retire_base;
                if(mover_done) mover_seen<=1;
                if(clear) clear_done<=0;
                else if(z_done) clear_done<=1;
                if(list_frame_done) frame_done<=1;
                if(!error && first_event) begin
                    error<=1;
                    error_code<=timeout_event ? EC_ERR_TIMEOUT : list_error ? list_code : core_code;
                    error_index<=timeout_event ? {16'd0,list_active_index} : {16'd0,list_error ? list_index:core_index};
                    error_tag<=timeout_event ? list_active_tag : list_error ? list_tag:core_tag;
                end
                if(finish || (soft_reset && quiet)) begin selected<=0;started<=0;end
            end
`ifdef GFX_HAS_RAST_TEX
            case({samp_valid && samp_ready,samp_rvalid})
                2'b10:samples<=samples+1'b1;
                2'b01:samples<=samples-1'b1;
                default:begin end
            endcase
`endif
        end
    end
    wire rp_we,rp_valid;
    wire [1:0] rp_slot,rp_slot_v,rp_free;
    wire [4:0] rp_wa,rp_strip;
    wire [127:0] rp_wd;
    wire [15:0] rp_index,rp_ord;
    engine_rlst u_rlst(.clk(clk),.rst(rst || (stopping && mover_seen)),.cancel(stopping),.go(go),
        .tri_base(tri_base_r),.sub_base(sub_base_r),.sub_bytes(bytes_r[23:0]),.tri_count(count_r[15:0]),
        .fd_addr(fd_addr),.fd_bytes(fd_bytes),.fd_cidx(fd_cidx),.fd_raster(),
        .fd_valid(fd_valid),.fd_ready(fd_ready),.fs_data(fs_data),.fs_valid(fs_valid),
        .fs_last(fs_last),.fs_keep(fs_keep),.fs_cidx(fs_cidx),.fs_ready(fs_ready),
        .st_valid(stat_valid),.st_error(stat_error),.st_cidx(stat_cidx),
        .rp_we(rp_we),.rp_slot(rp_slot),.rp_wa(rp_wa),.rp_wd(rp_wd),
        .rp_valid(rp_valid),.rp_slot_v(rp_slot_v),.rp_index(rp_index),.rp_strip(rp_strip),
        .rp_ordinal(rp_ord),.rp_free(rp_free),.strip_req(list_strip_req),.cur_strip(cur_strip),
        .strip_ack(strip_ack),.strip_done(strip_done),.frame_done(list_frame_done),
        .error(list_error),.error_code(list_code),.error_index(list_index),
        .error_tag(list_tag),.busy(rlst_busy),.active_index(list_active_index),.active_tag(list_active_tag));
    // After quiet the core is reset locally to release err_hold before another frame.
    engine_rast #(.TEX_LATENCY(TEX_LATENCY)) u_rast(.clk(clk),.rst(rst || (stopping && quiet)),
        .cancel(stopping),.rp_we(rp_we),.rp_slot(rp_slot),.rp_wa(rp_wa),.rp_wd(rp_wd),
        .rp_valid(rp_valid),.rp_slot_v(rp_slot_v),.rp_index(rp_index),.rp_strip(rp_strip),
        .rp_ordinal(rp_ord),.rp_free(rp_free),.ras_wr_valid(ras_valid),.ras_wr_pix(ras_pix),
        .ras_wr_data(ras_data),.ras_wr_ready(ras_ready && ras_lease),
        .z_clear(clear),.z_clear_busy(z_busy),.z_clear_done(z_done),.idle(core_idle),
`ifdef GFX_HAS_RAST_TEX
        .samp_valid(samp_valid),.samp_ready(samp_ready),.samp_word(samp_word),
        .samp_rvalid(samp_rvalid),.samp_rdata(samp_rdata),
        .tex_width(13'd64),.tex_height(13'd64),.tex_slog2(4'd6),.ras_key(key_r),
`endif
        .err_valid(core_error),.err_code(core_code),.err_index(core_index),
        .err_tag(core_tag),.dbg_retire_cnt(retired));
`ifndef GFX_HAS_RAST_TEX
    assign samp_valid=0;assign samp_word=0;
`endif
endmodule
`default_nettype wire
