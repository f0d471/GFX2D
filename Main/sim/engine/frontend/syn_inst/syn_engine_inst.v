// 全输入动态、全输出可观察，防止独立综合裁掉坐标 RAM 与检查通路。
module syn_engine_inst(input wire clk,rst,output reg [127:0] obs);
    reg [127:0] r;
    always @(posedge clk)if(rst)r<=128'h123456789abcdef123456789abcdef12;
    else r<={r[126:0],r[127]^r[125]^r[100]^obs[0]};
    wire [31:0] addr,xy,uv,iid;
    wire [11:0] bytes;
    wire [3:0] ctx,code;
    wire busy,req,fr,valid,error;
    engine_inst u_inst(.clk(clk),.rst(rst),.cancel(r[0]),.go(r[1]),.base(r[63:32]),
        .count(r[79:64]),.stride(r[88:80]),.layout(r[89]),.fixed_uv(r[31:0]),.context_id(r[93:90]),
        .busy(busy),.fd_addr(addr),.fd_bytes(bytes),.fd_cidx(ctx),.fd_valid(req),.fd_ready(r[94]),
        .fs_data(r),.fs_valid(r[95]),.fs_last(r[96]),.fs_cidx(r[100:97]),.fs_ready(fr),
        .stat_valid(r[101]),.stat_error(r[102]),.stat_cidx(r[106:103]),.valid(valid),.ready(r[107]),
        .xy(xy),.uv(uv),.instance_id(iid),.error(error),.error_code(code));
    always @(posedge clk)obs<={addr,xy,uv,iid}^{87'd0,bytes,ctx,code,busy,req,fr,valid,error,16'd0};
endmodule
