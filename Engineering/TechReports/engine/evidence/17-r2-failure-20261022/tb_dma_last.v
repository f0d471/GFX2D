`timescale 1ns/1ps
module tb_dma_last;
reg clk=0,rst=1,dv=0;always #5 clk=~clk;
wire dr,arv,rr,tv,tl,stat;
wire[7:0] arlen;
wire[3:0] status_error;
reg active=0;reg[8:0] left=0;
wire rv=active;
wire rl=active&&left==1;
axi_dma_rd #(.AXI_DATA_WIDTH(128),.AXI_ADDR_WIDTH(32),.AXI_ID_WIDTH(4),
.AXIS_DATA_WIDTH(128),.AXIS_KEEP_ENABLE(1),.AXIS_LAST_ENABLE(1),
.AXIS_ID_WIDTH(1),.AXIS_DEST_WIDTH(1),.AXIS_USER_WIDTH(1),
.LEN_WIDTH(12),.TAG_WIDTH(4),.ENABLE_SG(0),.ENABLE_UNALIGNED(0)) d(
.clk(clk),.rst(rst),.enable(1'b1),
.s_axis_read_desc_addr(32'd0),.s_axis_read_desc_len(12'd128),.s_axis_read_desc_tag(4'd0),
.s_axis_read_desc_id(1'b0),.s_axis_read_desc_dest(1'b0),.s_axis_read_desc_user(1'b0),
.s_axis_read_desc_valid(dv),.s_axis_read_desc_ready(dr),
.m_axis_read_desc_status_valid(stat),.m_axis_read_desc_status_error(status_error),
.m_axis_read_data_tvalid(tv),.m_axis_read_data_tready(1'b1),.m_axis_read_data_tlast(tl),
.m_axi_arlen(arlen),.m_axi_arvalid(arv),.m_axi_arready(1'b1),
.m_axi_rid(4'd0),.m_axi_rdata(128'd0),.m_axi_rresp(2'd0),
.m_axi_rlast(rl),.m_axi_rvalid(rv),.m_axi_rready(rr));
always @(posedge clk)begin
if(rst)begin active<=0;left<=0;end
else if(arv)begin active<=1;left<={1'b0,arlen}+1'b1;end
else if(rv&&rr)begin left<=left-1'b1;if(left==1)active<=0;end
end
integer beats=0,lasts=0;
always @(posedge clk)begin
if(!rst&&tv)begin
beats=beats+1;if(tl)lasts=lasts+1;
$display("DMA_STREAM beat=%0d last=%b",beats,tl);
if(beats==8)begin $display("DMA_LAST_SUMMARY beats=%0d last_count=%0d",beats,lasts);#1;$finish;end
end
end
initial begin repeat(3)@(negedge clk);rst=0;while(!dr)@(negedge clk);dv=1;@(negedge clk);dv=0;end
initial begin #10000;$display("DMA_LAST_TIMEOUT");$finish;end
endmodule
