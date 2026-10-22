
`timescale 1ns/1ps
module tb_micro;
reg clk=0,rst=1;
always #5 clk=~clk;
engine_rast d(.clk(clk),.rst(rst),.cancel(1'b0),.rp_we(1'b0),.rp_slot(2'd0),.rp_wa(5'd0),.rp_wd(128'd0),.rp_valid(1'b0),.rp_slot_v(2'd0),.rp_index(16'd0),.rp_strip(5'd0),.ras_wr_ready(1'b1),.z_clear(1'b0));
integer i;
task reset;
begin
  rst=1; repeat(2) @(negedge clk); rst=0;
  for(i=0;i<32;i=i+1) d.slot0[i]=0;
  d.act_sel=0; d.i0v=0; d.i_cur=0; d.i_end=1; d.j_cur=0; d.j_end=1;
  d.jms=0;d.row_base=0;d.e_cur0=32'hfffffff0;d.e_cur1=32'd16;d.e_cur2=32'd16;
  d.bias_pos0=1;d.bias_pos1=1;d.bias_pos2=1;
end
endtask
initial begin
reset;
#1 $display("PROBE_COVER negative_edge=%h actual_cov=%b expected=0",d.e_cur0,d.cov_now);
d.slot0[11]=32'hffff0000; d.acc0=48'd6553600; d.state=3;d.scan_active=1;
@(posedge clk);#1;
$display("PROBE_NEGATIVE_GRADIENT actual_acc=%0d expected=6488064 increment_width=%0d",d.acc0,$bits(d.attr_gx[0]));
@(negedge clk);
reset;
d.slot0[11]=32'd65536;d.slot0[12]=32'd655360;
d.acc0=48'd6619136;d.i_cur=1;d.i_end=1;d.state=3;d.scan_active=1;
@(posedge clk);#1;
$display("PROBE_ROW_START actual_acc=%0d expected=7208960",d.acc0);
@(negedge clk);
reset;
d.state=2;d.init_cnt=11; d.i0v=200;d.i1v=201;d.j0v=26;d.j1v=27;d.jmsv=10;
d.c_sum[0]=-48'sd16;for(i=1;i<6;i=i+1)d.c_sum[i]=0;
for(i=0;i<3;i=i+1)d.c_er[i]=1;
@(posedge clk);#1;
$display("PROBE_INITIAL_OFFSET pix=%0d expected=6600 jms=%0d",d.pix_now,d.jms);
$display("PROBE_NEGATIVE_INIT actual=%h expected=ffffffffffff",d.acc0);
@(negedge clk);
reset;
d.state=3;d.scan_active=1;d.i_cur=1;d.i_end=1;d.j_cur=0;d.j_end=0;
@(posedge clk);#1;
$display("PROBE_TERMINAL_COUNTER j=%0d end=%0d active=%b fire=%b expected_no_more_fire",d.j_cur,d.j_end,d.scan_active,d.fire0);
repeat(3)begin @(posedge clk);#1;if(d.fire0)$display("PROBE_UNEXPECTED_EXTRA_FIRE");end
$finish;
end
endmodule
