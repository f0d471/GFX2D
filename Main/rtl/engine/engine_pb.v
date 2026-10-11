// F5 帧控制与 PB 引用执行。构建失败先排空，不进入 LOAD；渲染失败不发布 DONE。
`timescale 1ns/1ps
`default_nettype none
module engine_pb #(parameter integer STRIP_H=16)(
    input wire clk,rst,go,band_mode,
    input wire [31:0] list_base,list_count,pb_base,pb_bytes,draw_cap,ref_cap,lstrips,strip_log2,band_surfs,
    output wire busy,building,abort,
    output reg done,failed,
    output reg [3:0] error_code,error_stage,
    output reg [31:0] error_index,error_instance,error_tag,
    output wire perf_cmd,perf_instance,
    output wire [1:0] ref_add,
    output wire [3:0] src_id,dst_id,
    input wire [31:0] src_base,src_dim,src_ctrl,dst_base,dst_dim,dst_ctrl,
    output wire band_start,band_flush,
    output reg [31:0] bg_base,target_base,
    output reg [4:0] bg_slog2,target_slog2,
    output wire [15:0] band_y0,
    output reg [15:0] frame_height,
    input wire band_start_ready,band_flush_ready,band_loaded,band_flushed,band_busy,band_error,
    input wire [3:0] band_error_code,
    input wire drain_done,tmo_evt,tmo_write,
    output wire command_go,
    output wire [255:0] command_packet,
    output wire [63:0] command_clip,
    input wire command_busy,command_done,command_error,
    input wire [3:0] command_error_code,
    output wire [31:0] fd_addr,
    output wire [11:0] fd_bytes,
    output wire [3:0] fd_cidx,
    output wire fd_instance,fd_valid,
    input wire fd_ready,
    input wire [127:0] fs_data,
    input wire fs_valid,fs_last,fs_instance,
    input wire [3:0] fs_cidx,
    output wire fs_ready,
    input wire rd_status,rd_error,rd_instance,
    input wire [3:0] rd_cidx,
    output wire [31:0] pw_addr,
    output wire [11:0] pw_bytes,
    output wire pw_valid,
    input wire pw_ready,
    output wire [127:0] pw_data,
    output wire pw_data_valid,
    input wire pw_data_ready,pw_status,pw_error
);
    localparam IDLE=0,CFG_BG=1,CFG_DST=2,VALIDATE=3,BUILD=4,LOAD=5,LOADED=6,
        REF=7,GOT_REF=8,META=9,GOT_META=10,CHECK=11,COORD=12,GOT_COORD=13,
        UV=14,GOT_UV=15,EXEC=16,EXEC_WAIT=17,NEXT=18,FLUSH=19,FLUSHED=20,
        WAIT=21,ERROR_DRAIN=22,FINISH=23,START_BUILD=24,REF_VALUE=25,COORD_VALUE=26,CHECK_LATCH=27,
        ERROR_WRITE=28,ERROR_WAIT=29,CHECK_INPUT=30;
    reg [4:0] state,ret;
    reg [31:0] list_r,pb_r,bytes_r;
    reg [15:0] count16,draw16;reg [17:0] refs18;
    wire [31:0] count_r={16'd0,count16},draw_r={16'd0,draw16},refs_r={14'd0,refs18};
    reg [5:0] strips_r;
    reg [3:0] target,bg_id_r;
    reg [4:0] strip;
    reg [17:0] ref18;reg [15:0] packet16;
    wire [31:0] ref_index={14'd0,ref18},packet_index={16'd0,packet16};
    reg [31:0] instance_index,xy,uv;
    reg [255:0] packet;
    reg [63:0] snapshot;
    reg [15:0] meta_index;reg [31:0] coord_cache_addr;
    reg [127:0] ref_cache,coord_cache;
    reg meta_valid,ref_valid,coord_valid;
    reg workspace_valid;
    reg request,write;
    reg [31:0] address;
    reg [6:0] bytes;
    wire [511:0] write_data={128'd0,{28'd0,error_stage},error_instance,error_index,{28'd0,error_code},
        ref_offset,32'd512,total_refs,packets,{26'd0,strips_r},32'd3,32'd1,32'h31425046};
    wire io_ready,io_done,io_error;
    wire [127:0] read_beat;
    wire [1:0] read_index;
    wire read_valid;
    reg meta_bad;
    wire bin_busy,bin_done,bin_failed;
    reg bin_go;
    wire [3:0] bin_error_code,bin_error_stage,bin_src,bin_dst;
    wire [31:0] bin_error_index,bin_error_instance,bin_error_tag,packets,total_refs,ref_offset,dir_count,dir_offset;
    wire [31:0] active_index,active_instance,active_tag;
    wire [1:0] active_write_context;
    wire bin_request,bin_write;
    wire [31:0] bin_address;
    wire [6:0] bin_bytes;
    wire [511:0] bin_write_data;
    wire [31:0] inst_addr;
    wire [11:0] inst_bytes;
    wire [3:0] inst_cidx;
    wire inst_valid,inst_fs_ready;
    wire [31:0] dma_addr;
    wire [11:0] dma_bytes;
    wire dma_valid,dma_fs_ready;
    wire [3:0] packet_error;
    reg [3:0] packet_error_q;
    wire [32:0] cfg_dst_end={1'b0,dst_base}+(({17'd0,dst_dim[31:16]}-33'd1)<<dst_ctrl[4:0])+33'd2560;
    wire [32:0] cfg_bg_end={1'b0,src_base}+(({17'd0,src_dim[31:16]}-33'd1)<<src_ctrl[4:0])+33'd2560;
    reg [32:0] bg_end,target_end;
    wire [37:0] input_end={6'd0,list_r}+{1'd0,count_r,5'd0};
    wire [32:0] pb_end={1'b0,pb_r}+{1'b0,bytes_r};
    wire [32:0] required=33'd512+(({1'b0,draw_r}+33'd1)&~33'd1)*33'd64+({1'b0,refs_r}<<3)+({27'd0,strips_r}<<4);
    wire bg_ok,dst_ok;
    ec_struct_ok u_surface(.s0(src_base),.s1(src_dim),.s2(src_ctrl),.ok(bg_ok));
    ec_struct_ok u_target(.s0(dst_base),.s1(dst_dim),.s2(dst_ctrl),.ok(dst_ok));
    function overlap;
        input [47:0] a,ae,b,be;
        begin overlap=a<be && b<ae;end
    endfunction
    assign busy=state!=IDLE;
    assign building=state==START_BUILD || state==BUILD;
    assign abort=state==ERROR_DRAIN;
    assign src_id=state==CFG_BG?bg_id_r:bin_busy?bin_src:packet[19:16];
    assign dst_id=state==CFG_DST?target:bin_busy?bin_dst:packet[23:20];
    assign band_start=state==LOAD;
    assign band_flush=state==FLUSH;
    assign band_y0={11'd0,strip}*STRIP_H;
    assign command_go=state==EXEC && !command_busy;
    assign command_packet=packet[7:0]==5?
        {packet[255:224],32'd0,packet[159:128],32'd0,packet[127:96],xy,uv,packet[31:8],8'd2}:packet;
    assign command_clip=snapshot;
    assign fd_instance=inst_valid;
    assign fd_addr=inst_valid?inst_addr:dma_addr;
    assign fd_bytes=inst_valid?inst_bytes:dma_bytes;
    assign fd_cidx=inst_valid?inst_cidx:4'd0;
    assign fd_valid=inst_valid||dma_valid;
    assign fs_ready=fs_instance?inst_fs_ready:dma_fs_ready;
    engine_bin #(.STRIP_H(STRIP_H)) u_bin(.clk(clk),.rst(rst||abort),.go(bin_go),
        .input_base(list_r),.input_count(count_r),.pb_base(pb_r),.pb_bytes(bytes_r),.draw_cap(draw_r),.ref_cap(refs_r),
        .strips(strips_r),.target(target),.bg_base(bg_base),.bg_bytes(bg_end[31:0]-bg_base),
        .busy(bin_busy),.done(bin_done),.failed(bin_failed),.error_code(bin_error_code),.error_stage(bin_error_stage),
        .error_index(bin_error_index),.error_instance(bin_error_instance),.error_tag(bin_error_tag),
        .active_index(active_index),.active_instance(active_instance),.active_tag(active_tag),
        .write_context(active_write_context),
        .perf_cmd(perf_cmd),.perf_instance(perf_instance),.ref_add(ref_add),
        .packets(packets),.total_refs(total_refs),.reference_offset(ref_offset),
        .directory_id(strip),.directory_count(dir_count),.directory_offset(dir_offset),
        .src_id(bin_src),.dst_id(bin_dst),.src_base(src_base),.src_dim(src_dim),.src_ctrl(src_ctrl),
        .dst_base(dst_base),.dst_dim(dst_dim),.dst_ctrl(dst_ctrl),
        .play_packet(packet),.play_error(packet_error),
        .request(bin_request),.write(bin_write),.address(bin_address),.bytes(bin_bytes),.write_data(bin_write_data),
        .io_ready(io_ready),.io_done(io_done),.io_error(io_error),
        .read_beat(read_beat),.read_index(read_index),.read_valid(read_valid&&bin_busy),
        .inst_fd_addr(inst_addr),.inst_fd_bytes(inst_bytes),.inst_fd_cidx(inst_cidx),.inst_fd_valid(inst_valid),.inst_fd_ready(fd_ready&&!dma_valid),
        .fs_data(fs_data),.fs_valid(fs_valid&&fs_instance),.fs_last(fs_last),.fs_cidx(fs_cidx),.inst_fs_ready(inst_fs_ready),
        .inst_status(rd_status&&rd_instance),.inst_status_error(rd_error),.inst_status_cidx(rd_cidx));
    engine_pb_dma u_dma(.clk(clk),.rst(rst||abort),
        .request(bin_busy?bin_request:request),.write(bin_busy?bin_write:write),
        .address(bin_busy?bin_address:address),.bytes(bin_busy?bin_bytes:bytes),.write_data(bin_busy?bin_write_data:write_data),
        .ready(io_ready),.done(io_done),.error(io_error),.read_beat(read_beat),.read_index(read_index),.read_valid(read_valid),
        .fd_addr(dma_addr),.fd_bytes(dma_bytes),.fd_valid(dma_valid),.fd_ready(fd_ready&&!inst_valid),
        .fs_data(fs_data),.fs_valid(fs_valid&&!fs_instance),.fs_last(fs_last),.fs_ready(dma_fs_ready),
        .rd_status(rd_status&&!rd_instance),.rd_error(rd_error),
        .pw_addr(pw_addr),.pw_bytes(pw_bytes),.pw_valid(pw_valid),.pw_ready(pw_ready),
        .pw_data(pw_data),.pw_data_valid(pw_data_valid),.pw_data_ready(pw_data_ready),.pw_status(pw_status),.pw_error(pw_error));
    wire [31:0] ref_address=pb_r+ref_offset+dir_offset+(ref_index<<3);
    wire [32:0] directory_end={1'b0,ref_offset}+{1'b0,dir_offset}+
        ((({1'b0,dir_count}<<3)+33'd15)&~33'd15);
    wire clip_ok=snapshot==64'hffffffff00000000 ||
        (!snapshot[15]&&!snapshot[31]&&!snapshot[47]&&!snapshot[63] &&
         snapshot[15:0]<=snapshot[47:32] && snapshot[31:16]<=snapshot[63:48] &&
         snapshot[47:32]<=639 && snapshot[63:48]<=479);
    wire [63:0] reference=ref_index[0]?ref_cache[127:64]:ref_cache[63:0];
    // CHECK 已验证 instance_index<count<=65535、stride<=256，位宽按合法域收敛。
    wire [24:0] coord_offset=instance_index[15:0]*packet[168:160];
    wire [32:0] coord_address={1'b0,packet[63:32]}+{8'd0,coord_offset};
    wire [31:0] coord_word=coord_cache[coord_address[3:2]*32+:32];
    wire [1:0] uv_lane=coord_address[3:2]+2'd1;
    wire [31:0] uv_word=coord_cache[uv_lane*32+:32];
    task io;
        input [31:0] a;
        input [6:0] n;
        input [4:0] next;
        begin request<=1;write<=0;address<=a;bytes<=n;ret<=next;state<=WAIT;end
    endtask
    task fault;
        input [3:0] code,stage;
        begin failed<=1;error_code<=code;error_stage<=stage;error_index<=packet_index;
            error_instance<=instance_index;error_tag<=packet[255:224];state<=ERROR_DRAIN;end
    endtask
    always @(posedge clk) begin
        if(rst)begin state<=IDLE;done<=0;failed<=0;request<=0;bin_go<=0;end
        else begin
            done<=0;request<=0;bin_go<=0;
            if(read_valid && !bin_busy && state==WAIT)begin
                if(ret==GOT_REF)ref_cache<=read_beat;
                if(ret==GOT_COORD || ret==GOT_UV)coord_cache<=read_beat;
                if(ret==GOT_UV)uv<=read_beat[31:0];
                if(ret==GOT_META)begin
                    if(read_index==0)packet[127:0]<=read_beat;
                    if(read_index==1)packet[255:128]<=read_beat;
                    if(read_index==2)begin
                        snapshot<=read_beat[63:0];
                        if(read_beat[95:64]!=packet_index || read_beat[127:96]!=0)meta_bad<=1;
                    end
                    if(read_index==3 && read_beat!=0)meta_bad<=1;
                end
            end
            if(tmo_evt && busy && state!=ERROR_DRAIN && state!=FINISH)begin
                if(failed)state<=ERROR_DRAIN;
                else begin
                    fault(12,building?(tmo_write?4'd4:(fs_instance?4'd2:4'd1)):4'd5);
                    if(building)begin
                        error_index<=(tmo_write&&active_write_context==0)?32'd0:active_index;
                        error_instance<=tmo_write?(active_write_context==2?active_instance:32'hffffffff):
                            (fs_instance?active_instance:32'hffffffff);
                        error_tag<=tmo_write?(active_write_context!=0?active_tag:32'd0):(fs_instance?active_tag:32'd0);
                    end
                end
            end
            else case(state)
                IDLE: if(go)begin
                    list_r<=list_base;count16<=list_count[15:0];pb_r<=pb_base;bytes_r<=pb_bytes;draw16<=draw_cap[15:0];refs18<=ref_cap[17:0];
                    strips_r<=lstrips[5:0];target<=band_surfs[3:0];bg_id_r<=band_surfs[7:4];strip<=0;
                    packet16<=0;instance_index<=32'hffffffff;packet<=0;failed<=0;
                    error_code<=0;error_stage<=0;error_index<=0;error_tag<=0;error_instance<=32'hffffffff;
                    meta_valid<=0;ref_valid<=0;coord_valid<=0;
                    workspace_valid<=0;
                    if(!band_mode || lstrips==0 || lstrips>30 || strip_log2!=$clog2(STRIP_H) || band_surfs[31:8]!=0)
                        begin failed<=1;error_code<=2;error_stage<=1;state<=ERROR_DRAIN;end
                    else if(draw_cap==0 || draw_cap>32768 || ref_cap>131072 || list_count>draw_cap)
                        begin failed<=1;error_code<=2;error_stage<=3;state<=ERROR_DRAIN;end
                    else state<=CFG_BG;
                end
                CFG_BG: if(!bg_ok || src_base[3:0]!=0 || src_dim[15:0]!=640 || src_ctrl[15:8]!=0)
                    fault(4,1);
                    else begin bg_base<=src_base;bg_slog2<=src_ctrl[4:0];bg_end<=cfg_bg_end;frame_height<=src_dim[31:16];state<=CFG_DST;end
                CFG_DST: if(!dst_ok || dst_base[3:0]!=0 || dst_dim[15:0]!=640 || dst_ctrl[15:8]!=0 ||
                    dst_dim[31:16]!=frame_height || {16'd0,frame_height}!=strips_r*STRIP_H || frame_height>480 ||
                    (overlap({16'd0,bg_base},bg_end,{16'd0,dst_base},cfg_dst_end) &&
                    (bg_base!=dst_base || bg_slog2!=dst_ctrl[4:0])))fault(4,1);
                    else begin target_base<=dst_base;target_slog2<=dst_ctrl[4:0];target_end<=cfg_dst_end;state<=VALIDATE;end
                VALIDATE: if(pb_r[6:0]!=0 || draw_r==0 || draw_r>32768 || refs_r>131072 || count_r>draw_r ||
                    required>{1'b0,bytes_r} || pb_end>33'h100000000 || input_end>38'h100000000 ||
                    (count_r!=0&&overlap({16'd0,pb_r},{15'd0,pb_end},{16'd0,list_r},{10'd0,input_end})) ||
                    overlap({16'd0,pb_r},{15'd0,pb_end},{16'd0,bg_base},bg_end) ||
                    overlap({16'd0,pb_r},{15'd0,pb_end},{16'd0,target_base},target_end) ||
                    (count_r!=0&&overlap({16'd0,list_r},{10'd0,input_end},{16'd0,target_base},target_end)))fault(2,3);
                    else if(count_r!=0 && list_r[4:0]!=0)fault(9,1);
                    else begin workspace_valid<=1;bin_go<=1;state<=START_BUILD;end
                START_BUILD: state<=BUILD;
                BUILD: if(bin_done)begin
                    if(bin_failed)begin
                        failed<=1;error_code<=bin_error_code;error_stage<=bin_error_stage;
                        error_index<=bin_error_index;error_instance<=bin_error_instance;error_tag<=bin_error_tag;state<=ERROR_DRAIN;
                    end else begin strip<=0;state<=LOAD;end
                end
                LOAD: if(band_start_ready)state<=LOADED;
                LOADED: if(band_error)fault(band_error_code,6);
                    else if(band_loaded)begin
                        if(dir_count>refs_r || dir_offset[3:0]!=0 || directory_end>{1'b0,bytes_r})begin
                            fault(9,5);error_index<=0;error_instance<=32'hffffffff;error_tag<=0;
                        end else begin ref18<=0;ref_valid<=0;state<=REF;end
                    end
                REF: if(ref_index>=dir_count)state<=FLUSH;
                    // 每桶从 16 B 对齐地址顺序消费；奇数引用复用刚读入拍的高半。
                    else if(ref_valid && ref_index[0])state<=REF_VALUE;
                    else if(io_ready)io({ref_address[31:4],4'd0},16,GOT_REF);
                GOT_REF:begin ref_valid<=1;state<=REF_VALUE;end
                REF_VALUE: if(reference[31:0]>=packets)begin
                    fault(2,5);error_index<=0;error_instance<=32'hffffffff;error_tag<=0;
                end
                    else begin packet16<=reference[15:0];instance_index<=reference[63:32];state<=META;end
                META: if(meta_valid && meta_index==packet_index)state<=CHECK_INPUT;
                    else if(io_ready)begin meta_bad<=0;io(pb_r+512+(packet_index<<6),64,GOT_META);end
                GOT_META: if(meta_bad)fault(2,5);
                    else begin
                        meta_index<=packet16;meta_valid<=1;state<=CHECK_INPUT;
                    end
                CHECK_INPUT:state<=CHECK_LATCH;
                CHECK_LATCH:begin packet_error_q<=packet_error;state<=CHECK;end
                CHECK: if(!clip_ok)begin fault(2,5);error_instance<=32'hffffffff;end
                    else if(packet_error_q!=0)begin fault(packet_error_q,5);error_instance<=32'hffffffff;end
                    else if(!(packet[7:0]==1 || packet[7:0]==2 || packet[7:0]==5) ||
                        ((packet[7:0]==5)!=(instance_index!=32'hffffffff)) ||
                        (packet[7:0]==5 && instance_index>={16'd0,packet[79:64]}))begin fault(2,5);error_instance<=32'hffffffff;end
                    else state<=packet[7:0]==5?COORD:EXEC;
                COORD: if(coord_address>33'hfffffffc)fault(9,5);
                    else if(coord_valid && coord_cache_addr=={coord_address[31:4],4'd0})state<=COORD_VALUE;
                    else if(io_ready)io({coord_address[31:4],4'd0},16,GOT_COORD);
                GOT_COORD:begin coord_cache_addr<={coord_address[31:4],4'd0};coord_valid<=1;state<=COORD_VALUE;end
                COORD_VALUE:begin
                    xy<=coord_word;
                    if(!packet[176])begin uv<=packet[223:192];state<=EXEC;end
                    else if(coord_address[3:0]<=8)begin uv<=uv_word;state<=EXEC;end
                    else state<=UV;
                end
                UV: if(io_ready)io({coord_address[31:4],4'd0}+16,16,GOT_UV);
                GOT_UV:begin coord_cache_addr<={coord_address[31:4],4'd0}+16;state<=EXEC;end
                EXEC: if(!command_busy)state<=EXEC_WAIT;
                EXEC_WAIT: if(command_error)fault(command_error_code,5);
                    else if(command_done)state<=NEXT;
                NEXT:begin ref18<=ref18+18'd1;state<=REF;end
                FLUSH: if(band_flush_ready)state<=FLUSHED;
                FLUSHED: if(band_error)fault(band_error_code,6);
                    else if(band_flushed)begin
                        if({1'b0,strip}+1==strips_r)state<=FINISH;
                        else begin strip<=strip+5'd1;state<=LOAD;end
                    end
                WAIT: if(io_done)begin
                    if(io_error)begin
                        fault((ret==GOT_COORD||ret==GOT_UV)?4'd10:4'd9,5);
                        if(ret==GOT_REF)begin error_index<=0;error_instance<=32'hffffffff;error_tag<=0;end
                        if(ret==GOT_META)begin error_instance<=32'hffffffff;error_tag<=0;end
                    end else state<=ret;
                end
                ERROR_DRAIN: if(drain_done)state<=workspace_valid?ERROR_WRITE:FINISH;
                ERROR_WRITE:if(io_ready)begin
                    workspace_valid<=0;request<=1;write<=1;address<=pb_r;bytes<=64;
                    state<=ERROR_WAIT;
                end
                ERROR_WAIT:if(io_done)state<=FINISH;
                FINISH:begin done<=1;state<=IDLE;end
                default:state<=IDLE;
            endcase
        end
    end
endmodule
`default_nettype wire
