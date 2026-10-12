// 研究适配器：TEX/静态源代理与紧凑 BAND，真实 geom/pix 在外部。
// 此模块没有 APB、ABI、DMA 或排空实现，不是正式 BAND/mover。
// 背景装载/写回由测试台拷贝；真实 DMA 三相搬运另由 tb_probe_band_dma 验证。
`timescale 1ns/1ps
`default_nettype none
module probe_fixture_memory;
    reg [127:0] mem[0:32767];
    integer wlast_err=0;
endmodule
module probe_band_local #(parameter integer WRITE_STALL=0)(
    input wire clk,rst,
    input wire [31:0] fb_base,
    input wire [15:0] strip_y0,
    input wire [31:0] rd_addr,wr_addr,
    input wire [11:0] rd_bytes,wr_bytes,
    input wire [1:0] rd_buf,
    input wire [3:0] rd_cidx,wr_cidx,
    input wire rd_tex,rd_valid,wr_valid,wr_last_row,
    output wire rd_ready,wr_ready,
    output reg [127:0] rs_tdata,
    output wire [15:0] rs_tkeep,
    output reg rs_tvalid,rs_tlast,
    output reg [1:0] rs_buf,
    output reg rd_stat_valid,wr_stat_valid,wr_stat_last_row,
    output wire rd_stat_err,wr_stat_err,
    output reg [3:0] rd_stat_cidx,wr_stat_cidx,
    input wire [127:0] ws_tdata,
    input wire [15:0] ws_tkeep,
    input wire ws_tvalid,
    output wire ws_tready
);
    reg [127:0] fixture[0:32767];
    reg [127:0] tex[0:1023];
    reg [127:0] band[0:2559];
    reg rb=0,wb=0,read_tail=0;
    reg [31:0] rp,wp;
    reg [11:0] rleft,wleft;
    reg [1:0] rbuf;
    reg rtex,wlast;
    reg [3:0] ticks=0;
    integer p,k,idx,collision=0;
    function integer index_band;
        input [31:0] a;
        reg [31:0] off;
        begin
            off=a-fb_base;
`ifdef INJ_BAND_ROUTE
            index_band=((off>>12)-strip_y0)*256+((off&4095)>>4);
`else
            index_band=((off>>12)-strip_y0)*160+((off&4095)>>4);
`endif
        end
    endfunction
    assign rd_ready=!rb;
    assign wr_ready=!wb;
    assign ws_tready=wb && (WRITE_STALL==0 || ticks[2:0]!=0);
    assign rs_tkeep=16'hffff;
    assign rd_stat_err=1'b0;
    assign wr_stat_err=1'b0;
    always @(posedge clk) begin
        if(rst) begin
            ticks<=0;
            rb<=0;wb<=0;read_tail<=0;rs_tvalid<=0;rs_tlast<=0;
            rd_stat_valid<=0;wr_stat_valid<=0;
            rs_tdata<=0;rs_buf<=0;rd_stat_cidx<=0;wr_stat_cidx<=0;wr_stat_last_row<=0;
            rp<=0;wp<=0;rleft<=0;wleft<=0;rbuf<=0;rtex<=0;wlast<=0;
        end else begin
            ticks<=ticks+1;
            rs_tvalid<=0;rs_tlast<=0;rd_stat_valid<=0;wr_stat_valid<=0;
            if(rd_valid && rd_ready) begin
                rb<=1;rp<=rd_addr;rleft<=rd_bytes>>4;rbuf<=rd_buf;rtex<=rd_tex;
                rd_stat_cidx<=rd_cidx;read_tail<=0;
            end
            if(rb && rleft!=0) begin
                if(rbuf[1]) begin
                    idx=index_band(rp);
                    if(idx<0 || idx>=2560) begin
                        $display("BAND-BOUNDS read idx=%0d",idx);rs_tdata<=0;
                    end else rs_tdata<=band[idx];
                end else if(rtex) rs_tdata<=tex[rp[13:4]];
                else rs_tdata<=fixture[rp[18:4]];
                rs_tvalid<=1;rs_tlast<=rleft==1;rs_buf<=rbuf;
                rp<=rp+16;rleft<=rleft-1;
                if(rleft==1) read_tail<=1;
            end
            if(read_tail) begin rd_stat_valid<=1;read_tail<=0;rb<=0;end
            if(wr_valid && wr_ready) begin
                wb<=1;wp<=wr_addr;wleft<=wr_bytes>>4;
                wr_stat_cidx<=wr_cidx;wlast<=wr_last_row;
            end
            if(ws_tvalid && ws_tready) begin
                idx=index_band(wp);
                if(idx<0 || idx>=2560) $display("BAND-BOUNDS write idx=%0d",idx);
                else begin
                    if(rb && rleft!=0 && rbuf[1] && index_band(rp)==idx) begin
                        collision=collision+1;$display("BAND-COLLISION idx=%0d",idx);
                    end
                    for(p=0;p<4;p=p+1) begin
`ifdef INJ_BAND_LOCAL_MASK
                        if(1'b1) begin
`else
                        if(!ws_tdata[p*32+31]) begin
`endif
                            for(k=0;k<4;k=k+1) if(ws_tkeep[p*4+k])
                                band[idx][p*32+k*8 +:8]<=k==3 ? 8'd0 : ws_tdata[p*32+k*8 +:8];
                        end
                    end
                end
                wp<=wp+16;wleft<=wleft-1;
                if(wleft==1) begin wb<=0;wr_stat_valid<=1;wr_stat_last_row<=wlast;end
            end
        end
    end
endmodule
`default_nettype wire
