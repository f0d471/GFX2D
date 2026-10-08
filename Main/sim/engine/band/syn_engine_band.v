// 正式 BAND 独立综合：输入均为变量，全部输出进入观测；不代表整机时序。
`timescale 1ns/1ps
`default_nettype none
module syn_engine_band(input wire clk,rst,output reg obs);
    reg [511:0] stim;
    wire sr,fr,loaded,flushed,active,busy,loading,flushing,error;
    wire [3:0] error_code;
    wire [31:0] dra,dwa;wire [11:0] drn,dwn;
    wire drv,dwv,drready,dwvalid;wire [127:0] dwdata,rsdata;
    wire rr,wr,rsv,rsl,rstat,rerr,wstat,werr,wlast,rbusy,wbusy,wsready;
    wire [1:0] rsbuf,rstatbuf;wire [3:0] rtag,wtag;
    wire [4:0] bgsl=5'd12+{3'd0,stim[5:4]},dstsl=5'd12+{3'd0,stim[7:6]};
    wire [15:0] y0={7'd0,stim[12:8],4'd0},height=16'd480-{8'd0,stim[20:13]};
    wire [11:0] rn={1'b0,stim[27:21],4'd0}+12'd16;
    wire [11:0] wn={1'b0,stim[34:28],4'd0}+12'd16;
    always @(posedge clk) begin
        if(rst) stim<=512'h195aeb53d02b127832477512014650917ffffe329a82;
        else stim<={stim[510:0],stim[511]^stim[509]^stim[506]^stim[503]};
        obs<=^{sr,fr,loaded,flushed,active,busy,loading,flushing,error,error_code,
               dra,dwa,drn,dwn,drv,dwv,drready,dwvalid,
               (dwvalid ? dwdata : 128'd0),(rsv ? rsdata : 128'd0),
               rr,wr,rsv,rsl,rstat,rerr,wstat,werr,wlast,rbusy,wbusy,wsready,rsbuf,rstatbuf,rtag,wtag};
    end
    engine_band dut(.clk(clk),.rst(rst),.cancel(stim[35]),.err_clr(stim[36]),
        .start(stim[37]),.start_ready(sr),.flush(stim[38]),.flush_ready(fr),
        .bg_base({16'd0,stim[50:39],4'd0}),.dst_base({16'd0,stim[62:51],4'd0}),
        .bg_slog2(bgsl),.dst_slog2(dstsl),.strip_y0(y0),.frame_height(height),
        .loaded(loaded),.flushed(flushed),.active(active),.busy(busy),.loading(loading),.flushing(flushing),
        .error(error),.error_code(error_code),
        .dma_rd_addr(dra),.dma_rd_bytes(drn),.dma_rd_valid(drv),.dma_rd_ready(stim[63]),
        .dma_rdata(stim[191:64]),.dma_rvalid(stim[192]),.dma_rlast(stim[193]),.dma_rready(drready),
        .dma_rd_done(stim[194]),.dma_rd_error(stim[195]),
        .dma_wr_addr(dwa),.dma_wr_bytes(dwn),.dma_wr_valid(dwv),.dma_wr_ready(stim[196]),
        .dma_wdata(dwdata),.dma_wvalid(dwvalid),.dma_wready(stim[197]),
        .dma_wr_done(stim[198]),.dma_wr_error(stim[199]),.dma_idle(stim[200]),
        .rd_addr({stim[228:201],4'd0}),.rd_bytes(rn),.rd_buf(stim[230:229]),.rd_cidx(stim[234:231]),
        .rd_valid(stim[235]),.rd_ready(rr),.rs_data(rsdata),.rs_valid(rsv),.rs_last(rsl),.rs_buf(rsbuf),
        .rd_stat_valid(rstat),.rd_stat_error(rerr),.rd_stat_cidx(rtag),.rd_stat_buf(rstatbuf),
        .rd_stat_ready(stim[236]),.rd_busy(rbusy),
        .wr_addr({stim[264:237],4'd0}),.wr_bytes(wn),.wr_cidx(stim[268:265]),.wr_last_row(stim[269]),
        .wr_valid(stim[270]),.wr_ready(wr),.ws_data(stim[398:271]),.ws_keep(stim[414:399]),
        .ws_valid(stim[415]),.ws_ready(wsready),.wr_stat_valid(wstat),.wr_stat_error(werr),
        .wr_stat_cidx(wtag),.wr_stat_last_row(wlast),.wr_stat_ready(stim[416]),.wr_busy(wbusy));
endmodule
`default_nettype wire
