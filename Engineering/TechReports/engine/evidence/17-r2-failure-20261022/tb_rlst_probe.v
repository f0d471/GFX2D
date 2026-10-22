
`timescale 1ns/1ps
module tb_rlst_probe;
reg clk=0,rst=1,go=0;
always #5 clk=~clk;
wire fv,fr,fl,sv,sd,done;wire[11:0] bytes;wire[31:0] addr;
engine_rlst d(.clk(clk),.rst(rst),.cancel(1'b0),.go(go),.tri_base(32'h800000),.sub_base(32'h100000),.sub_bytes(24'd120),.tri_count(16'd1),.fd_addr(addr),.fd_bytes(bytes),.fd_valid(fv),.fd_ready(1'b1),.fs_data(128'd0),.fs_valid(1'b0),.fs_last(1'b0),.st_valid(1'b0),.st_error(1'b0),.rp_free(2'b11),.strip_ack(1'b1),.strip_req(sv),.strip_done(sd),.frame_done(done));
initial begin
repeat(3)@(negedge clk);rst=0;go=1;@(negedge clk);go=0;
repeat(10)@(negedge clk);
$display("PROBE_SUB120 state=%0d fd_valid=%b env=%0d expected_env=128 reserved_term=%0d expected_entries=60 reserve=%b",d.state,fv,d.env_total,d.s_chunk_valid[6:0],d.s_reserve_ok);
force d.state=1;force d.env_total=25'd143;force d.env_left=25'd143;force d.fifo_wp=7'd1;force d.fifo_rp=7'd0;
// sub_bytes 保持 120，先只展示剩余小包络产生零长度的组合边界。
force d.env_left=25'd7;
#1;$display("PROBE_ZERO_FD env_left=%0d chunk=%0d beats=%0d fd_bytes=%0d",d.env_left,d.s_chunk,d.s_chunk_beats,bytes);
force d.env_total=25'd655;force d.env_left=25'd639;force d.s_recv=4'd1;
#1;$display("PROBE_DOUBLE_OFFSET done=%0d recv=%0d b_rel_next=%0d expected=16",d.s_logic_done,d.s_recv,d.b_rel_next);
release d.env_left; release d.env_total;release d.s_recv;release d.state;
force d.state=13;force d.r_frag_final=0;d.r_addr=32'h800fa0;force d.r_frag_len=8'd96;force d.canceled=0;force d.r_bad=0;force d.r_stat_err=0;
@(posedge clk);#1;

$display("PROBE_SPLIT_ADDRESS actual=%h expected=00801000",d.r_addr);
$finish;
end
endmodule
