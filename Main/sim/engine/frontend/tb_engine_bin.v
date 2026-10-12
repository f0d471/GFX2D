// 两遍构建：一万张独立软件 PB 金标准，逐字核对目录/metadata/引用/padding。
`timescale 1ns/1ps
`default_nettype none
module tb_engine_bin;
    reg clk=0,rst=1,go=0;always #5 clk=~clk;
    reg [31:0] input_count;
    wire busy,done,failed,request,write;
    wire [3:0] code,stage,src_id,dst_id;
    wire [31:0] error_index,error_instance,error_tag,packets,total_refs,ref_offset,address;
    wire [6:0] bytes;
    wire [511:0] write_data;
    reg io_done=0,io_error=0;
    reg [127:0] read_beat=0;reg [1:0] read_index=0;reg read_valid=0;
    wire [31:0] fd_addr;
    wire [11:0] fd_bytes;
    wire [3:0] fd_cidx;
    wire fd_valid,fs_ready;
    reg stream=0,status_pending=0,inst_status=0;
    reg [31:0] stream_addr;
    integer stream_beat,stream_beats,stream_gap,status_gap;
    reg [3:0] stream_ctx;
    wire fd_ready=!stream&&!status_pending;
    wire fs_valid=stream&&stream_gap==0;
    wire fs_last=stream_beat+1==stream_beats;
    reg [31:0] arena[0:2047];
    reg [255:0] cmds[0:63];
    reg [127:0] pb_mem[0:4607];
    wire [127:0] fs_data={arena[((stream_addr-32'h30000000)>>2)+stream_beat*4+3],
        arena[((stream_addr-32'h30000000)>>2)+stream_beat*4+2],
        arena[((stream_addr-32'h30000000)>>2)+stream_beat*4+1],
        arena[((stream_addr-32'h30000000)>>2)+stream_beat*4]};
    engine_bin u_bin(.clk(clk),.rst(rst),.go(go),.input_base(32'h40000000),.input_count(input_count),
        .pb_base(32'h10000000),.pb_bytes(32'd71040),.draw_cap(32'd64),.ref_cap(32'd8192),
        .strips(6'd30),.target(4'd0),.bg_base(32'h20000000),.bg_bytes(32'h1dfa00),
        .busy(busy),.done(done),.failed(failed),.error_code(code),.error_stage(stage),
        .error_index(error_index),.error_instance(error_instance),.error_tag(error_tag),
        .perf_cmd(),.perf_instance(),.ref_add(),.packets(packets),.total_refs(total_refs),.reference_offset(ref_offset),
        .directory_id(5'd0),.directory_count(),.directory_offset(),.src_id(src_id),.dst_id(dst_id),
        .src_base(32'h201e0000),.src_dim(32'h00200020),.src_ctrl(32'h80000007),
        .dst_base(32'h20000000),.dst_dim(32'h01e00280),.dst_ctrl(32'h8000000c),
        .play_packet(256'd0),.play_error(),
        .request(request),.write(write),.address(address),.bytes(bytes),.write_data(write_data),
        .io_ready(!io_pending),.io_done(io_done),.io_error(io_error),.read_beat(read_beat),.read_index(read_index),.read_valid(read_valid),
        .inst_fd_addr(fd_addr),.inst_fd_bytes(fd_bytes),.inst_fd_cidx(fd_cidx),.inst_fd_valid(fd_valid),.inst_fd_ready(fd_ready),
        .fs_data(fs_data),.fs_valid(fs_valid),.fs_last(fs_last),.fs_cidx(stream_ctx),.inst_fs_ready(fs_ready),
        .inst_status(inst_status),.inst_status_error(1'b0),.inst_status_cidx(stream_ctx));
    reg [31:0] random=32'h45a19623;
    reg io_pending=0,io_write;
    reg [31:0] io_addr;
    reg [6:0] io_bytes;
    reg [511:0] io_data;
    integer io_delay,io_word,j;
    reg early_ready=0;
    always @(posedge clk)begin
        random<=(random^(random<<13))^((random^(random<<13))>>17);
        io_done<=0;inst_status<=0;read_valid<=0;
        if(!rst)begin
            if(request&&!io_pending)begin
                if(write && address==32'h10000000 && write_data[95:64]==2 &&
                    !(u_bin.state==14 && u_bin.ret==23 && u_bin.header_chunk==0))early_ready<=1;
                io_pending<=1;io_write<=write;io_addr<=address;io_bytes<=bytes;io_data<=write_data;io_delay<=random[2:0];io_word<=0;
            end
            if(io_pending)begin
                if(io_delay>0)io_delay<=io_delay-1;
                else begin
                    if(io_write)begin
                        for(j=0;j<4;j=j+1)if(j*16<io_bytes)pb_mem[((io_addr-32'h10000000)>>4)+j]<=io_data[j*128+:128];
                        io_done<=1;io_pending<=0;
                    end else begin
                        if(io_addr>=32'h40000000)read_beat<=cmds[(io_addr-32'h40000000)>>5][io_word*128+:128];
                        else read_beat<=pb_mem[((io_addr-32'h10000000)>>4)+io_word];
                        read_index<=io_word[1:0];read_valid<=1;
                        if((io_word+1)*16==io_bytes)begin io_done<=1;io_pending<=0;end
                        else begin io_word<=io_word+1;io_delay<=random[1:0];end
                    end
                end
            end
            if(fd_valid&&fd_ready)begin
                stream<=1;stream_addr<=fd_addr;stream_beats<=fd_bytes/16;stream_beat<=0;stream_gap<=random[2:0];stream_ctx<=fd_cidx;
            end
            if(stream)begin
                if(stream_gap>0)stream_gap<=stream_gap-1;
                else if(fs_ready)begin
                    if(fs_last)begin stream<=0;status_pending<=1;status_gap<=random[1:0];end
                    else begin stream_beat<=stream_beat+1;stream_gap<=random[1:0];end
                end
            end
            if(status_pending)begin
                if(status_gap>0)status_gap<=status_gap-1;
                else begin inst_status<=1;status_pending<=0;end
            end
        end
    end
    integer f,scenes,k,n,an,used,refs,span,rc,i,w,guard,limit,errors=0;
    reg [127:0] gold;
    reg [511:0] meta;
    reg [1023:0] filename;
    initial begin
        if(!$value$plusargs("CASES=%s",filename))filename="Main/sim/engine/frontend/build/f5/bin_cases.dat";
        f=$fopen(filename,"r");if(!f)$fatal(1,"missing cases");
        rc=$fscanf(f,"%d",scenes);
        if($value$plusargs("LIMIT=%d",limit) && limit<scenes)scenes=limit;
        repeat(5)@(negedge clk);rst=0;
        for(k=0;k<scenes;k=k+1)begin
            rc=$fscanf(f,"%d %d %d %d",n,an,used,refs);
            for(i=0;i<n;i=i+1)rc=$fscanf(f,"%h",cmds[i]);
            for(i=0;i<2048;i=i+1)arena[i]=0;
            for(i=0;i<an;i=i+1)rc=$fscanf(f,"%h",arena[i]);
            for(i=0;i<4608;i=i+1)pb_mem[i]={4{32'hdeadc0de}};
            @(negedge clk);input_count=n;go=1;@(negedge clk);go=0;
            guard=0;
            while(!done&&guard<100000)begin @(negedge clk);guard=guard+1;end
            if(!done||failed||early_ready||packets!=used||total_refs!=refs)begin
                $display("FAIL scene=%d failed=%b code=%d stage=%d index=%d state=%d",k,failed,code,stage,error_index,u_bin.state);errors=errors+1;
            end
            for(i=0;i<32;i=i+1)begin
                rc=$fscanf(f,"%h",gold);
                if(pb_mem[i]!==gold)begin if(errors<10)$display("FAIL header scene=%d beat=%d got=%h exp=%h",k,i,pb_mem[i],gold);errors=errors+1;end
            end
            for(i=0;i<used;i=i+1)begin
                rc=$fscanf(f,"%h",meta);
                for(w=0;w<4;w=w+1)if(pb_mem[32+i*4+w]!==meta[w*128+:128])begin
                    if(errors<10)$display("FAIL metadata scene=%d slot=%d beat=%d",k,i,w);errors=errors+1;
                end
            end
            rc=$fscanf(f,"%d",span);
            for(i=0;i<span;i=i+1)begin
                rc=$fscanf(f,"%h",gold);
                if(pb_mem[ref_offset/16+i]!==gold)begin if(errors<10)$display("FAIL ref scene=%d beat=%d",k,i);errors=errors+1;end
            end
            if(errors)$fatal(1,"F5 BIN model mismatch");
            if(k%1000==999)$display("F5 BIN progress %d",k+1);
        end
        $fclose(f);$display("RESULT: PASS F5 BIN scenes=%d errors=%d",scenes,errors);$finish;
    end
endmodule
`default_nettype wire
