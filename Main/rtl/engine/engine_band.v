// engine_band：G2 条带数据面，640×STRIP_H×32 位、单缓冲。
//
// 端口分相：LOAD 的 DMA 流写 RAM；EXEC 的本地目的读/像素写使用一读一写；
// FLUSH 的同步读输出保持到 DMA ready。局部地址从全帧逻辑 stride 转成紧凑词号。
// 四像素写使能 + 最大二次幂段/余段组织，STRIP_H=16 时为 2048+512，研究实测 36 RAM。
// 像素 bit31 是不写标记、可写 X 清 0；原始背景/flush 数据完整保留 32 位。
//
// cancel 不直接复位 DMA：停止请求/本地流，等 dma_idle 后回 EMPTY。
// 已接受的 DDR 事务由 mover 排空；RAM 内容不复位。局部状态保持到 ready，
// flush 等本地读/写数据与状态均收尾，下一条带不能提早覆盖。
// G3 使用 start/flush 和描述符口；本模块不解释命令 ABI、不遍历帧级命令表。
// 依据：TechReports engine 第 08 章、P3 总览 D58/D62。
`timescale 1ns/1ps
`default_nettype none
module engine_band #(
    parameter integer WIDTH_PIXELS = 640,
    parameter integer STRIP_H = 16
)(
    input wire clk, rst, cancel, err_clr,
    input wire start,
    output wire start_ready,
    input wire flush,
    output wire flush_ready,
    input wire [31:0] bg_base, dst_base,
    input wire [4:0] bg_slog2, dst_slog2,
    input wire [15:0] strip_y0, frame_height,
    output reg loaded, flushed,
    output wire active, busy, loading, flushing,
    output reg error,
    output reg [3:0] error_code,

    output wire [31:0] dma_rd_addr,
    output wire [11:0] dma_rd_bytes,
    output wire dma_rd_valid,
    input wire dma_rd_ready,
    input wire [127:0] dma_rdata,
    input wire dma_rvalid, dma_rlast,
    output wire dma_rready,
    input wire dma_rd_done, dma_rd_error,
    output wire [31:0] dma_wr_addr,
    output wire [11:0] dma_wr_bytes,
    output wire dma_wr_valid,
    input wire dma_wr_ready,
    output wire [127:0] dma_wdata,
    output wire dma_wvalid,
    input wire dma_wready,
    input wire dma_wr_done, dma_wr_error, dma_idle,

    input wire [31:0] rd_addr,
    input wire [11:0] rd_bytes,
    input wire [1:0] rd_buf,
    input wire [3:0] rd_cidx,
    input wire rd_valid,
    output wire rd_ready,
    output wire [127:0] rs_data,
    output wire rs_valid, rs_last,
    output wire [1:0] rs_buf,
    output reg rd_stat_valid, rd_stat_error,
    output reg [3:0] rd_stat_cidx,
    output reg [1:0] rd_stat_buf,
    input wire rd_stat_ready,
    output wire rd_busy,

    input wire [31:0] wr_addr,
    input wire [11:0] wr_bytes,
    input wire [3:0] wr_cidx,
    input wire wr_last_row, wr_valid,
    output wire wr_ready,
    input wire [127:0] ws_data,
    input wire [15:0] ws_keep,
    input wire ws_valid,
    output wire ws_ready,
    output reg wr_stat_valid, wr_stat_error,
    output reg [3:0] wr_stat_cidx,
    output reg wr_stat_last_row,
    input wire wr_stat_ready,
    output wire wr_busy
);
    localparam integer ROW_WORDS=WIDTH_PIXELS/4;
    localparam integer ROW_BYTES=WIDTH_PIXELS*4;
    localparam integer WORDS=ROW_WORDS*STRIP_H;
    localparam integer AW=$clog2(WORDS);
    localparam integer BIG_WORDS=1<<($clog2(WORDS)-1);
    localparam integer SMALL_WORDS=WORDS-BIG_WORDS;
    localparam integer BIG_AW=$clog2(BIG_WORDS);
    localparam integer SMALL_AW=$clog2(SMALL_WORDS);
    localparam [3:0] EMPTY=0, LOAD_REQ=1, LOAD_DATA=2, LOAD_WAIT=3,
                     EXEC=4, FLUSH_REQ=5, FLUSH_DATA=6, FLUSH_WAIT=7, CANCEL=8;
    localparam [3:0] E_CFG=1, E_RANGE=2, E_READ=3, E_WRITE=4, E_LENGTH=5;
    reg [3:0] phase;
    reg [31:0] r_bg, r_dst, bg_cursor, dst_cursor;
    reg [4:0] r_slog;
    reg [15:0] r_y0, row;
    reg [31:0] bg_stride, dst_stride;
    reg [11:0] load_left;
    reg [AW-1:0] load_index, flush_index;
    reg load_seen, load_failed;
    reg [11:0] flush_issued;
    reg flush_out_valid;
    reg r_reading, w_writing;
    reg rd_pending, wr_pending;
    reg [31:0] rd_addr_q, wr_addr_q;
    reg [11:0] rd_bytes_q, wr_bytes_q;
    reg [11:0] read_left, write_left;
    reg [AW-1:0] read_index, write_index;
    reg read_v, read_l;
    reg [1:0] read_buf;
    wire [127:0] ram_q;

    wire [31:0] cfg_last_y={16'd0,strip_y0}+STRIP_H-1;
    wire [47:0] cfg_bg_end={16'd0,bg_base}+({16'd0,cfg_last_y}<<bg_slog2)+ROW_BYTES-1;
    wire [47:0] cfg_dst_end={16'd0,dst_base}+({16'd0,cfg_last_y}<<dst_slog2)+ROW_BYTES-1;
    wire cfg_ok=(WIDTH_PIXELS>0 && WIDTH_PIXELS<=1020 && WIDTH_PIXELS%4==0) &&
                (STRIP_H>0 && STRIP_H<=32 && (STRIP_H&(STRIP_H-1))==0) &&
                bg_base[3:0]==0 && dst_base[3:0]==0 &&
                (32'd1<<bg_slog2)>=ROW_BYTES && (32'd1<<dst_slog2)>=ROW_BYTES &&
                frame_height!=0 && frame_height<=480 && cfg_last_y<{16'd0,frame_height} &&
                cfg_bg_end[47:32]==0 && cfg_dst_end[47:32]==0;
    wire start_fire=start && start_ready;
    wire flush_fire=flush && flush_ready;
    assign start_ready=(phase==EMPTY) && !cancel;
`ifdef INJ_BAND_EARLY_FLUSH
    assign flush_ready=(phase==EXEC) && !cancel;
`else
    assign flush_ready=(phase==EXEC) && !rd_busy && !wr_busy && !cancel;
`endif
    assign active=phase==EXEC;
    assign busy=phase!=EMPTY;
    assign loading=phase==LOAD_REQ || phase==LOAD_DATA || phase==LOAD_WAIT;
    assign flushing=phase==FLUSH_REQ || phase==FLUSH_DATA || phase==FLUSH_WAIT;
    assign dma_rd_addr=bg_cursor;
    assign dma_rd_bytes=ROW_BYTES;
    assign dma_rd_valid=phase==LOAD_REQ && !cancel;
    assign dma_rready=1'b1;
`ifdef INJ_BAND_FLUSH_ADDR
    assign dma_wr_addr=dst_cursor+32'd16;
    assign dma_wr_bytes=ROW_BYTES;
`else
    assign dma_wr_addr=dst_cursor;
    assign dma_wr_bytes=ROW_BYTES;
`endif
    assign dma_wr_valid=phase==FLUSH_REQ && !cancel;
    assign dma_wdata=ram_q;
    assign dma_wvalid=(phase==FLUSH_DATA) && flush_out_valid && !cancel;

    // 本地描述符先寄存一拍，再检查/转换；pending 计入 busy，flush 不得越过。
    // 该寄存切断 geom 的分段地址加法与 BAND 地址检查的跨模块组合长路径。
    // 完整描述符先检查再截断；逻辑 padding 不占 BAND 容量。
    function desc_ok;
        input [31:0] a;
        input [11:0] n;
        reg [31:0] off,yy,col;
        begin
            off=a-r_dst;yy=off>>r_slog;col=off&((32'd1<<r_slog)-1);
            desc_ok=a>=r_dst && a[3:0]==0 && n!=0 && n[3:0]==0 &&
                    yy>={16'd0,r_y0} && yy<({16'd0,r_y0}+STRIP_H) &&
                    ({1'b0,col}+{21'd0,n})<=ROW_BYTES;
        end
    endfunction
    function [AW-1:0] local_index;
        input [31:0] a;
        reg [31:0] off,yy,col,idx;
        begin
            off=a-r_dst;yy=(off>>r_slog)-{16'd0,r_y0};
            col=(off&((32'd1<<r_slog)-1))>>4;
`ifdef INJ_BAND_ADDR_STRIDE
            idx=(yy<<8)+col;
`else
            idx=yy*ROW_WORDS+col;
`endif
            local_index=idx[AW-1:0];
        end
    endfunction
    assign rd_busy=rd_pending || r_reading || read_v || rd_stat_valid;
    assign wr_busy=wr_pending || w_writing || wr_stat_valid;
    assign rd_ready=active && !rd_busy && !cancel && !flush_fire;
    assign wr_ready=active && !wr_busy && !cancel && !flush_fire;
    wire local_rd_fire=rd_valid && rd_ready;
    wire local_wr_fire=wr_valid && wr_ready;
    wire local_rd_bad=rd_pending && !desc_ok(rd_addr_q,rd_bytes_q);
    wire local_wr_bad=wr_pending && !desc_ok(wr_addr_q,wr_bytes_q);
    wire read_issue=active && r_reading && read_left!=0 && !cancel;
    wire flush_issue=(phase==FLUSH_DATA) && (!flush_out_valid || dma_wready) &&
                     flush_issued<ROW_WORDS && !cancel;
    wire ram_re=read_issue || flush_issue;
    wire [AW-1:0] ram_ra=read_issue ? read_index : flush_index;
    assign ws_ready=active && w_writing && !cancel &&
                    !(ram_re && ram_ra==write_index);
    wire pixel_write=ws_valid && ws_ready;
    // I4 的现有消费者输出全 keep；局部写不静默接收半字掩码。
    wire bad_keep=pixel_write && ws_keep!=16'hffff;
    wire fill_write=(phase==LOAD_DATA) && dma_rvalid && !cancel && load_left!=0;
    wire [AW-1:0] ram_wa=fill_write ? load_index : write_index;
    wire [127:0] ram_wd=fill_write ? dma_rdata :
        {8'd0,ws_data[119:96],8'd0,ws_data[87:64],8'd0,ws_data[55:32],8'd0,ws_data[23:0]};
    wire [3:0] ram_we;
    genvar p;
    generate for(p=0;p<4;p=p+1) begin : pixel_enable
`ifdef INJ_BAND_PIXEL_MASK
        assign ram_we[p]=!rst && (fill_write || (pixel_write && !bad_keep));
`else
        assign ram_we[p]=!rst && (fill_write ||
            (pixel_write && !bad_keep && !ws_data[32*p+31]));
`endif
    end endgenerate

    // 两段均为二次幂深度；段选择随同步输出锁存，反压时不推进。
    reg ram_sel;
    always @(posedge clk) if(ram_re) ram_sel<=ram_ra>=BIG_WORDS;
    generate for(p=0;p<4;p=p+1) begin : ram_lane
        reg [31:0] mem_big[0:BIG_WORDS-1];
        reg [31:0] mem_small[0:SMALL_WORDS-1];
        reg [31:0] q_big,q_small;
        always @(posedge clk) begin
            if(ram_we[p]) begin
                if(ram_wa>=BIG_WORDS) mem_small[ram_wa[SMALL_AW-1:0]]<=ram_wd[32*p +:32];
                else mem_big[ram_wa[BIG_AW-1:0]]<=ram_wd[32*p +:32];
            end
            if(ram_re) begin
                if(ram_ra>=BIG_WORDS) q_small<=mem_small[ram_ra[SMALL_AW-1:0]];
                else q_big<=mem_big[ram_ra[BIG_AW-1:0]];
            end
        end
        assign ram_q[32*p +:32]=ram_sel ? q_small : q_big;
    end endgenerate
    assign rs_data=ram_q;
    assign rs_valid=read_v && !cancel;
    assign rs_last=read_l;
    assign rs_buf=read_buf;

    always @(posedge clk) begin
        if(rst || cancel) begin
            rd_pending<=0;wr_pending<=0;rd_addr_q<=0;wr_addr_q<=0;rd_bytes_q<=0;wr_bytes_q<=0;
            r_reading<=0;read_v<=0;read_l<=0;read_left<=0;read_index<=0;read_buf<=0;
            rd_stat_valid<=0;rd_stat_error<=0;rd_stat_cidx<=0;rd_stat_buf<=0;
            w_writing<=0;write_left<=0;write_index<=0;
            wr_stat_valid<=0;wr_stat_error<=0;wr_stat_cidx<=0;wr_stat_last_row<=0;
        end else begin
            read_v<=read_issue;read_l<=read_issue && read_left==1;
            if(local_rd_fire) begin
                rd_addr_q<=rd_addr;rd_bytes_q<=rd_bytes;rd_pending<=1;
                read_buf<=rd_buf;rd_stat_buf<=rd_buf;rd_stat_cidx<=rd_cidx;
            end
            if(rd_pending) begin
                rd_pending<=0;rd_stat_error<=!desc_ok(rd_addr_q,rd_bytes_q);
                if(desc_ok(rd_addr_q,rd_bytes_q)) begin
                    r_reading<=1;read_left<=rd_bytes_q>>4;read_index<=local_index(rd_addr_q);
                end else rd_stat_valid<=1;
            end
            if(read_issue) begin read_index<=read_index+1'b1;read_left<=read_left-1'b1;end
            if(read_v && read_l) begin r_reading<=0;rd_stat_valid<=1;end
            if(rd_stat_valid && rd_stat_ready) rd_stat_valid<=0;
            if(local_wr_fire) begin
                wr_addr_q<=wr_addr;wr_bytes_q<=wr_bytes;wr_pending<=1;
                wr_stat_cidx<=wr_cidx;wr_stat_last_row<=wr_last_row;
            end
            if(wr_pending) begin
                wr_pending<=0;wr_stat_error<=!desc_ok(wr_addr_q,wr_bytes_q);
                if(desc_ok(wr_addr_q,wr_bytes_q)) begin
                    w_writing<=1;write_left<=wr_bytes_q>>4;write_index<=local_index(wr_addr_q);
                end else wr_stat_valid<=1;
            end
            if(pixel_write) begin
                write_index<=write_index+1'b1;write_left<=write_left-1'b1;
                if(bad_keep) wr_stat_error<=1;
                if(write_left==1) begin w_writing<=0;wr_stat_valid<=1;end
            end
            if(wr_stat_valid && wr_stat_ready) wr_stat_valid<=0;
        end
    end

    always @(posedge clk) begin
        if(rst) begin
            phase<=EMPTY;row<=0;r_bg<=0;r_dst<=0;r_slog<=0;r_y0<=0;
            bg_cursor<=0;dst_cursor<=0;bg_stride<=0;dst_stride<=0;
            load_left<=0;load_index<=0;load_seen<=0;load_failed<=0;
            flush_index<=0;flush_issued<=0;flush_out_valid<=0;
            loaded<=0;flushed<=0;
        end else begin
            loaded<=0;flushed<=0;
            if(cancel) begin
                phase<=dma_idle ? EMPTY : CANCEL;
                flush_out_valid<=0;load_seen<=0;
            end else case(phase)
                EMPTY: if(start_fire && cfg_ok) begin
                    r_bg<=bg_base;r_dst<=dst_base;r_slog<=dst_slog2;r_y0<=strip_y0;
                    bg_stride<=32'd1<<bg_slog2;dst_stride<=32'd1<<dst_slog2;
                    bg_cursor<=bg_base+({16'd0,strip_y0}<<bg_slog2);
                    dst_cursor<=dst_base+({16'd0,strip_y0}<<dst_slog2);
                    row<=0;load_index<=0;phase<=LOAD_REQ;
                end
                LOAD_REQ: if(dma_rd_valid && dma_rd_ready) begin
                    load_left<=ROW_WORDS;load_seen<=0;load_failed<=0;phase<=LOAD_DATA;
                end
                LOAD_DATA: begin
                    if(dma_rd_done) begin load_seen<=1;if(dma_rd_error) load_failed<=1;end
                    if(fill_write) begin
                        load_index<=load_index+1'b1;load_left<=load_left-1'b1;
                        if(dma_rlast!=(load_left==1)) load_failed<=1;
                        if(load_left==1 || dma_rlast) begin load_left<=0;phase<=LOAD_WAIT;end
                    end
                end
                LOAD_WAIT: begin
                    if(dma_rd_done) begin load_seen<=1;if(dma_rd_error) load_failed<=1;end
                    if(load_seen || dma_rd_done) begin
                        if(load_failed || (dma_rd_done && dma_rd_error)) phase<=EMPTY;
                        else if(row==STRIP_H-1) begin phase<=EXEC;loaded<=1;end
                        else begin row<=row+1'b1;bg_cursor<=bg_cursor+bg_stride;phase<=LOAD_REQ;end
                    end
                end
                EXEC: if(flush_fire) begin row<=0;flush_index<=0;phase<=FLUSH_REQ;end
                FLUSH_REQ: if(dma_wr_valid && dma_wr_ready) begin
                    flush_issued<=0;flush_out_valid<=0;phase<=FLUSH_DATA;
                end
                FLUSH_DATA: begin
                    if(!flush_out_valid || dma_wready) flush_out_valid<=flush_issue;
                    if(flush_issue) begin flush_index<=flush_index+1'b1;flush_issued<=flush_issued+1'b1;end
                    if(dma_wr_done) begin
                        flush_out_valid<=0;
                        if(dma_wr_error) phase<=EMPTY;
                        else if(row==STRIP_H-1) begin phase<=EMPTY;flushed<=1;end
                        else begin row<=row+1'b1;dst_cursor<=dst_cursor+dst_stride;phase<=FLUSH_REQ;end
                    end
                end
                FLUSH_WAIT: phase<=EMPTY;
                CANCEL: if(dma_idle) phase<=EMPTY;
                default: phase<=EMPTY;
            endcase
        end
    end
    always @(posedge clk) begin
        if(rst || err_clr || (start_fire && cfg_ok)) begin error<=0;error_code<=0;end
        else if(!error) begin
            if(start_fire && !cfg_ok) begin error<=1;error_code<=E_CFG;end
            else if(local_rd_bad || local_wr_bad) begin error<=1;error_code<=E_RANGE;end
            else if(loading && dma_rd_done && dma_rd_error) begin error<=1;error_code<=E_READ;end
            else if(flushing && dma_wr_done && dma_wr_error) begin error<=1;error_code<=E_WRITE;end
            else if(fill_write && dma_rlast!=(load_left==1)) begin error<=1;error_code<=E_LENGTH;end
            else if(bad_keep) begin error<=1;error_code<=E_LENGTH;end
        end
    end
endmodule
`default_nettype wire
