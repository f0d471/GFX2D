// 独立综合观测壳：动态移位寄存器驱动全部接口，512 位写数据分四页观察。
// 本壳只用于资源和时序测量；协议正确性由真实 DMA/DDR 测试台验证。
module syn_engine_bin(input wire clk,rst,output reg [127:0] obs);
    reg [2047:0] r;
    always @(posedge clk)if(rst)r<=2048'h51b00ff;
    else r<={r[2046:0],r[2047]^r[2034]^r[2015]^r[2000]};
    wire busy,done,failed,pc,pi,req,write,fdvalid,fsready;
    wire [1:0] refs_add;
    wire [3:0] code,stage,srcid,dstid,fdctx,play_error;
    wire [31:0] eindex,einstance,etag,aindex,ainstance,atag,packets,refs,refoff,dc,doff,addr,fdaddr;
    wire [6:0] bytes;
    wire [11:0] fdbytes;
    wire [511:0] data;
    engine_bin u_bin(.clk(clk),.rst(rst),.go(r[1317]),
        .input_base(r[31:0]),.input_count({16'd0,r[47:32]}),.pb_base(r[79:48]),.pb_bytes(r[111:80]),
        .draw_cap({16'd0,r[127:112]}),.ref_cap({14'd0,r[145:128]}),.strips(r[151:146]),.target(r[155:152]),
        .bg_base(r[187:156]),.bg_bytes(r[219:188]),.directory_id(r[224:220]),
        .src_base(r[256:225]),.src_dim(r[288:257]),.src_ctrl(r[320:289]),
        .dst_base(r[352:321]),.dst_dim(r[384:353]),.dst_ctrl(r[416:385]),
        .play_packet(r[672:417]),.play_error(play_error),.read_beat(r[800:673]),.read_index(r[802:801]),.read_valid(r[803]),
        .fs_data(r[1312:1185]),.fs_cidx(r[1316:1313]),.fs_valid(r[1318]),.fs_last(r[1319]),
        .inst_status(r[1320]),.inst_status_error(r[1321]),.inst_status_cidx(r[1316:1313]),
        .io_ready(r[1322]),.io_done(r[1323]),.io_error(r[1324]),.inst_fd_ready(r[1325]),
        .busy(busy),.done(done),.failed(failed),.error_code(code),.error_stage(stage),
        .error_index(eindex),.error_instance(einstance),.error_tag(etag),
        .active_index(aindex),.active_instance(ainstance),.active_tag(atag),
        .perf_cmd(pc),.perf_instance(pi),.ref_add(refs_add),.packets(packets),.total_refs(refs),.reference_offset(refoff),
        .directory_count(dc),.directory_offset(doff),.src_id(srcid),.dst_id(dstid),
        .request(req),.write(write),.address(addr),.bytes(bytes),.write_data(data),
        .inst_fd_addr(fdaddr),.inst_fd_bytes(fdbytes),.inst_fd_cidx(fdctx),.inst_fd_valid(fdvalid),.inst_fs_ready(fsready));
    wire [127:0] observed=r[2047:2046]==0?data[r[2045:2044]*128+:128]:
        r[2047:2046]==1?{addr,fdaddr,dc,doff}:
        r[2047:2046]==2?{eindex,einstance,etag,packets}:{aindex,ainstance,atag,refs};
    wire [79:0] status={refoff,code,stage,srcid,dstid,fdctx,bytes,busy,done,failed,pc,pi,req,write,fdvalid,fsready,refs_add,play_error};
    always @(posedge clk)if(rst)obs<=0;else obs<=observed^{48'd0,status}^{116'd0,fdbytes};
endmodule
