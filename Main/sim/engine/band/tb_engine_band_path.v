// 条带模式正式完整通路：真实 geom/pix/blend/mover/BAND/TEX 与两个 DMA 核。
// 软件执行器导出初态、命令和期望；按原序逐条提交并等末行写状态，逐拍比较 512 KiB。
// 覆盖 DDR/TEX 源、跨条带、负 x、重叠、有色色键、原始 X、取数并发及提前 flush。
// TIMED 使用校准从机；NO_FETCH 单独验证长行源的提前 DDR 返回；MODEL_SEED 改随机种子。
// 三相计数从 start 到 flushed，含直接控制器提交间隙，不含上传 TEX 和 SoC 扫描争用。
`timescale 1ns/1ps
`default_nettype none
// 注错开关以 -DINJ_xxx 直接传给被测 RTL（预处理器级宏）。
`ifdef INJ_SKIP_KEY
`define INJ_VAL 2
`else
`define INJ_VAL 0
`endif
`ifndef CASE
`define CASE "build/directed"
`endif
module tb_engine_band_path #(parameter integer MODEL_SEED=1, STRIP_H=16);
    reg [15:0] band_y0=0;reg band_start=0,band_flush=0,src_is_tex=0;
    wire band_start_ready,band_flush_ready,band_loaded,band_flushed,band_active,band_busy,band_error;
    wire [3:0] band_error_code;wire rd_band,wr_band;
    reg [13:0] tap_addr=0;reg tap_sel=0,tap_en=0;reg [31:0] tap_data=0;
    wire [13:0] tx_addr;wire [11:0] tx_bytes;wire [1:0] tx_buf,tx_rs_buf,tx_stat_buf;
    wire [3:0] tx_cidx,tx_stat_cidx;wire tx_valid,tx_ready,tx_busy,tx_rs_valid,tx_rs_last,tx_stat_valid,tx_stat_ready;wire [127:0] tx_rs_data;
    wire rd_stat_fetch;reg fd_valid=0;wire fd_ready,fs_valid,fs_last;wire [127:0] fs_data;wire [3:0] fs_cidx;
    integer fd_count=0,fd_stat_count=0,geom_done=0,early_attempts=0,ar_seen=0,aw_seen=0,preload_seen=0;

    reg clk=0, rst=1; always #5 clk=~clk;
    reg start=0;
    reg [7:0] op=0;
    reg [3:0] eq=0, sfmt=0, cidx=0;
    reg key_en=0;
    reg [31:0] key_w5=0, const_w4=0;
    reg [7:0] c=0;
    reg [31:0] src_base=0,dst_base=0;
    reg [15:0] src_w=0,src_h=0,dst_w=0,dst_h=0;
    reg [4:0] src_slog2=0,dst_slog2=0;
    reg signed [15:0] src_x=0,src_y=0,dst_x=0,dst_y=0;
    reg [15:0] width=0,height=0;
    wire geom_busy,cmd_done; wire [3:0] done_cidx;
    wire [1:0] row_free;
    wire [31:0] rd_addr,wr_addr;
    wire [11:0] rd_beats,wr_beats;
    wire [11:0] rd_bytes=rd_beats<<4,wr_bytes=wr_beats<<4;
    wire [1:0] rd_buf; wire [3:0] rd_cidx,wr_cidx;
    wire rd_valid,rd_ready,wr_valid,wr_ready,wr_last_row;
    wire rd_tex;
    wire [15:0] sb_row,sb_py;
    wire [10:0] sb_npx;wire [7:0] sb_beats,sb_op,sb_c;
    wire [1:0] sb_lead_dst,sb_lead_src;
    wire [3:0] sb_eq,sb_sfmt;
    wire sb_key_en,sb_read_dst,sb_valid,sb_ready;
    wire [31:0] sb_key,sb_w4;

    engine_geom #(.STRIP_H(STRIP_H)) u_geom(
        .clk(clk),.rst(rst),.start(start),.op(op),.eq(eq),
        .key_en(key_en),.key_w5(key_w5),.const_w4(const_w4),.c(c),
        .sfmt(sfmt),.src_base(src_base), .band_enable(1'b1), .band_y0(band_y0),.src_w(src_w),.src_h(src_h),
        .src_slog2(src_slog2),.src_tex(src_is_tex),.dst_base(dst_base),.dst_w(dst_w),
        .dst_h(dst_h),.dst_slog2(dst_slog2),.src_x(src_x),.src_y(src_y),
        .dst_x(dst_x),.dst_y(dst_y),.w(width),.h(height),
        .clip_x0(16'sd0),.clip_y0(16'sd0),
        .clip_x1(16'sd639),.clip_y1(16'sd479),.cidx(cidx),
        .row_free(row_free),.busy(geom_busy),.cmd_done(cmd_done),
        .done_cidx(done_cidx),
        .rd_addr(rd_addr),.rd_beats(rd_beats),.rd_buf(rd_buf),.rd_tex(rd_tex),.rd_band(rd_band),.wr_band(wr_band),
        .rd_cidx(rd_cidx),.rd_valid(rd_valid),.rd_ready(rd_ready),
        .wr_addr(wr_addr),.wr_beats(wr_beats),.wr_cidx(wr_cidx),
        .wr_last_row(wr_last_row),.wr_valid(wr_valid),.wr_ready(wr_ready),
        .sb_row(sb_row),.sb_py(sb_py),.sb_npx(sb_npx),.sb_beats(sb_beats),
        .sb_lead_dst(sb_lead_dst),.sb_lead_src(sb_lead_src),
        .sb_op(sb_op),.sb_eq(sb_eq),.sb_key_en(sb_key_en),
        .sb_key(sb_key),.sb_w4(sb_w4),.sb_c(sb_c),.sb_sfmt(sb_sfmt),
        .sb_read_dst(sb_read_dst),.sb_valid(sb_valid),.sb_ready(sb_ready));

    wire [127:0] rs_tdata,ws_tdata;
    wire [15:0] rs_tkeep,ws_tkeep;
    wire rs_tlast,rs_tvalid,rs_tready,ws_tvalid,ws_tready;
    wire [1:0] rs_buf;
    engine_pix u_pix(
        .clk(clk),.rst(rst),.sb_row(sb_row),.sb_npx(sb_npx),
        .sb_beats(sb_beats),.sb_lead_dst(sb_lead_dst),
        .sb_lead_src(sb_lead_src),.sb_op(sb_op),.sb_eq(sb_eq),
        .sb_key_en(sb_key_en),.sb_key(sb_key),.sb_w4(sb_w4),
        .sb_c(sb_c),.sb_sfmt(sb_sfmt),.sb_read_dst(sb_read_dst),
        .sb_valid(sb_valid),.sb_ready(sb_ready),.row_free(row_free),
        .rs_tdata(rs_tdata),.rs_tkeep(rs_tkeep),.rs_tlast(rs_tlast),
        .rs_tvalid(rs_tvalid),.rs_tready(rs_tready),.rs_buf(rs_buf),
        .ws_tdata(ws_tdata),.ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid),.ws_tready(ws_tready));

    wire rd_stat_valid,rd_stat_err,wr_stat_valid,wr_stat_err,wr_stat_last_row;
    wire [3:0] rd_stat_cidx,wr_stat_cidx;
    wire [1:0] rd_stat_buf;
    wire drain_busy,drain_done,tmo_evt,tmo_dir;
    wire [31:0] araddr,awaddr;
    wire [7:0] arlen,awlen;
    wire [2:0] arsize,awsize;
    wire [1:0] arburst,awburst;
    wire arvalid,arready,awvalid,awready;
    wire [127:0] rdata,wdata;
    wire [1:0] rresp,bresp;
    wire rlast,rvalid,rready,wlast,wvalid,wready,bvalid,bready;
    wire [15:0] wstrb;
    engine_mover #(.ENABLE_BAND(1),.BAND_STRIP_H(STRIP_H)) u_mover(
        .clk(clk),.rst(rst),
        .band_start_ready(band_start_ready),.band_flush_ready(band_flush_ready),
        .band_loaded(band_loaded),.band_flushed(band_flushed),.band_active(band_active),.band_busy(band_busy),
        .band_error(band_error),.band_error_code(band_error_code),.tx_stat_ready(tx_stat_ready),.rd_stat_fetch(rd_stat_fetch),
        .rd_addr(rd_addr),.rd_bytes(rd_bytes),.rd_buf(rd_buf),.rd_tex(rd_tex), .rd_band(rd_band), .wr_band(wr_band), .band_start(band_start), .band_flush(band_flush),
        .band_bg_base(32'h1000), .band_dst_base(32'h1000), .band_bg_slog2(5'd12), .band_dst_slog2(5'd12),
        .band_y0(band_y0), .band_frame_height(16'd96),
        .rd_cidx(rd_cidx),.rd_valid(rd_valid),.rd_ready(rd_ready),
        .tx_addr(tx_addr),.tx_bytes(tx_bytes),.tx_buf(tx_buf),.tx_cidx(tx_cidx),.tx_valid(tx_valid),
        .tx_ready(tx_ready),.tx_busy(tx_busy),
        .tx_rs_tdata(tx_rs_data),.tx_rs_tvalid(tx_rs_valid),.tx_rs_tlast(tx_rs_last),.tx_rs_buf(tx_rs_buf),
        .tx_stat_valid(tx_stat_valid),.tx_stat_cidx(tx_stat_cidx),.tx_stat_buf(tx_stat_buf),
        .fd_addr(32'h70000),.fd_bytes(12'd2048),.fd_cidx(4'd13),
        .fd_valid(fd_valid),.fd_ready(fd_ready),
        .rs_tdata(rs_tdata),.rs_tkeep(rs_tkeep),.rs_tlast(rs_tlast),
        .rs_tvalid(rs_tvalid),.rs_tready(rs_tready),.rs_buf(rs_buf),
        .fs_tdata(fs_data),.fs_tkeep(),.fs_tlast(fs_last),.fs_tvalid(fs_valid),
        .fs_tready(1'b1),.fs_cidx(fs_cidx),
        .wr_addr(wr_addr),.wr_bytes(wr_bytes),.wr_cidx(wr_cidx),
        .wr_last_row(wr_last_row),.wr_valid(wr_valid),.wr_ready(wr_ready),
        .ws_tdata(ws_tdata),.ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid),.ws_tready(ws_tready),
        .rd_stat_valid(rd_stat_valid),.rd_stat_err(rd_stat_err),
        .rd_stat_cidx(rd_stat_cidx),.rd_stat_buf(rd_stat_buf),
        .wr_stat_valid(wr_stat_valid),.wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx),.wr_stat_last_row(wr_stat_last_row),
        .drain_req(1'b0),.drain_busy(drain_busy),.drain_done(drain_done),
        .tmo_limit(13'd4096),.err_clr(1'b0),.tmo_evt(tmo_evt),
        .tmo_dir(tmo_dir),
        .m_axi_araddr(araddr),.m_axi_arlen(arlen),.m_axi_arsize(arsize),
        .m_axi_arburst(arburst),.m_axi_arvalid(arvalid),.m_axi_arready(arready),
        .m_axi_rdata(rdata),.m_axi_rresp(rresp),.m_axi_rlast(rlast),
        .m_axi_rvalid(rvalid),.m_axi_rready(rready),
        .m_axi_awaddr(awaddr),.m_axi_awlen(awlen),.m_axi_awsize(awsize),
        .m_axi_awburst(awburst),.m_axi_awvalid(awvalid),.m_axi_awready(awready),
        .m_axi_wdata(wdata),.m_axi_wstrb(wstrb),.m_axi_wlast(wlast),
        .m_axi_wvalid(wvalid),.m_axi_wready(wready),.m_axi_bresp(bresp),
        .m_axi_bvalid(bvalid),.m_axi_bready(bready));

`ifdef TIMED
    axi_ram_model #(.MEM_WORDS(32768),.STRB_EN(1),.STALL(0),.TIMED(1),
                    .RD_LAT(23),.RD_BUB(12),.WR_BUB(6),.B_LAT(1)) u_ram(
`else
    axi_ram_model #(.MEM_WORDS(32768),.STRB_EN(1),.STALL(3),.SEED(MODEL_SEED),
                    .ADDR_STALL_MAX(3)) u_ram(
`endif
        .clk(clk),.rst_n(!rst),
        .awaddr(awaddr),.awlen(awlen),.awvalid(awvalid),.awready(awready),
        .wdata(wdata),.wstrb(wstrb),.wlast(wlast),.wvalid(wvalid),
        .wready(wready),.bresp(bresp),.bvalid(bvalid),.bready(bready),
        .araddr(araddr),.arlen(arlen),.arvalid(arvalid),.arready(arready),
        .rdata(rdata),.rresp(rresp),.rlast(rlast),.rvalid(rvalid),
        .rready(rready),.hold_aw(1'b0),.hold_ar(1'b0));


    engine_tex u_tex(.clk(clk),.rst(rst),.paddr(tap_addr),.psel(tap_sel),.penable(tap_en),.pwrite(1'b1),.pwdata(tap_data),
        .prdata(),.pready(),.pslverr(),.req_addr(tx_addr),.req_bytes(tx_bytes),.req_buf(tx_buf),.req_cidx(tx_cidx),
        .req_valid(tx_valid),.req_ready(tx_ready),.rs_tdata(tx_rs_data),.rs_tvalid(tx_rs_valid),.rs_tlast(tx_rs_last),
        .rs_buf(tx_rs_buf),.stat_valid(tx_stat_valid),.stat_ready(tx_stat_ready),.stat_cidx(tx_stat_cidx),.stat_buf(tx_stat_buf),.busy(tx_busy));
    reg [255:0] cmd_rom [0:95];
    reg [31:0] case_meta [0:2];
    reg [95:0] surf_rom [0:15];
    reg [127:0] expected [0:32767];
    reg [31:0] words [0:7];
    reg [95:0] ss,ds;
    integer done_count=0,desc_count=0,read_count=0,write_count=0;
    integer errors=0,stalls=0;
    integer row_beats [0:4095];
    integer row_in=0,row_out=0,remaining=0;
    always @(posedge clk) if(!rst) begin
        if(sb_valid && sb_ready) begin
            row_beats[row_in]=sb_beats;
            row_in=row_in+1;
        end
        if(ws_tvalid) begin
            if(remaining==0) begin
                if(row_out>=row_in) begin
                    errors=errors+1;$display("UNEXPECTED_WRITE_ROW");
                end else begin remaining=row_beats[row_out];row_out=row_out+1;end
            end
            if(ws_tready && remaining>0) remaining=remaining-1;
        end else if(remaining!=0) begin
            errors=errors+1;$display("WRITE_STREAM_GAP");
        end
        if(rd_stat_valid) begin
            read_count=read_count+1;
            if(rd_stat_err) begin errors=errors+1;$display("READ_ERROR");end
        end
        if(wr_stat_valid) begin
            write_count=write_count+1;
            if(wr_stat_last_row) done_count=done_count+1;
            if(wr_stat_err) begin errors=errors+1;$display("WRITE_ERROR");end
        end
        if(rs_tvalid && !rs_tready) begin errors=errors+1;$display("I3_BACKPRESSURE");end
        if(ws_tvalid && !ws_tready) stalls=stalls+1;
        if(tmo_evt) begin errors=errors+1;$display("TIMEOUT dir=%b",tmo_dir);end
        if(wr_valid && wr_ready) desc_count=desc_count+1;
    end

    task submit;
        input integer index;
        integer j,srcid,dstid;
        reg [31:0] w0;
        begin
            for(j=0;j<8;j=j+1) words[j]=cmd_rom[index][j*32 +:32];
            w0=words[0];srcid=w0[19:16];dstid=w0[23:20];
            ss=surf_rom[srcid];ds=surf_rom[dstid];
            @(negedge clk);
            op=w0[7:0];eq=w0[11:8];key_en=w0[12];c=w0[31:24];
            key_w5=words[5];const_w4=words[4];
            sfmt=ss[79:76];src_is_tex=ss[73:72]==1;
            src_base=ss[31:0];src_w=ss[47:32];src_h=ss[63:48];
            src_slog2=ss[68:64];
            dst_base=ds[31:0];dst_w=ds[47:32];dst_h=ds[63:48];
            dst_slog2=ds[68:64];
            src_x=words[1][15:0];src_y=words[1][31:16];
            dst_x=words[2][15:0];dst_y=words[2][31:16];
            width=words[3][15:0];height=words[3][31:16];
            cidx=index[3:0];start=1;
            @(negedge clk);start=0;
        end
    endtask


    always @(posedge clk) if(!rst) begin
        if(cmd_done) geom_done=geom_done+1;
        if(arvalid && arready) ar_seen=ar_seen+1;
        if(arvalid && arready && u_mover.b_rbusy && !u_mover.rd_ctx_cmd) preload_seen=preload_seen+1;
        if(awvalid && awready) aw_seen=aw_seen+1;
        if(band_error) begin $display("BAND_ERROR code=%0d",band_error_code);$display("RESULT: FAIL");$finish;end
        if(u_mover.band_rs_pix && u_mover.core_rs_pix) begin $display("BAND_FLOW_COLLISION");$display("RESULT: FAIL");$finish;end
        if(rd_stat_valid && rd_stat_fetch) begin fd_stat_count=fd_stat_count+1;if(rd_stat_cidx!=13) begin errors=errors+1;$display("FETCH_STATUS_TAG");end end
        if(fs_valid) begin
            if(fs_data!=={4{32'hdead0000+fd_count}} || fs_cidx!=13 || fs_last!=(fd_count==127)) begin errors=errors+1;$display("FETCH_DATA idx=%0d",fd_count);end
            fd_count=fd_count+1;
        end
    end
    initial begin
        wait(!rst);
`ifndef EMPTY_CASE
`ifndef NO_FETCH
        wait(u_mover.b_rsvalid && u_mover.g_band.u_band.read_left>32);
        @(negedge clk);fd_valid=1;@(posedge clk);while(!fd_ready) @(posedge clk);@(negedge clk);fd_valid=0;
`endif
`endif
    end
    initial begin
        wait(!rst);
`ifndef EMPTY_CASE
        wait(u_mover.b_wbusy && u_mover.b_bus_idle);
        @(negedge clk);band_flush=1;early_attempts=early_attempts+1;
        if(band_flush_ready) begin $display("EARLY_FLUSH_ACCEPTED");$display("RESULT: FAIL");$finish;end
        @(negedge clk);band_flush=0;
`endif
    end
    initial begin #200000000; $display("WATCHDOG");$finish;end

    integer ci,cy,mi,mem_bad,expected_done,t_start;
    integer cycle=0;
    integer strip,ci_geom_before,ci_write_before,has_pixels,upload;
    integer load_cycles=0,exec_cycles=0,flush_cycles=0;
    integer strip_load=0,strip_exec=0,strip_flush=0;
    reg [127:0] tex_rom[0:1023];
    always @(posedge clk) cycle=cycle+1;
    always @(posedge clk) if(!rst) begin
        if(u_mover.b_loading) load_cycles=load_cycles+1;
        if(band_active) exec_cycles=exec_cycles+1;
        if(u_mover.b_flushing) flush_cycles=flush_cycles+1;
    end
    initial begin
        $readmemh({`CASE,".cnt.hex"},case_meta);
        if(case_meta[0]<1 || case_meta[0]>96) begin $display("CASE_LENGTH");$display("RESULT: FAIL");$finish;end
        $readmemh({`CASE,".cmd.hex"},cmd_rom,0,case_meta[0]-1);
        $readmemh({`CASE,".surf.hex"},surf_rom);
        $readmemh({`CASE,".exp.hex"},expected);
        #1 $readmemh({`CASE,".init.hex"},u_ram.mem);
        repeat(5) @(negedge clk);rst=0;
        for(mi=0;mi<128;mi=mi+1) begin
            u_ram.mem[(32'h70000>>4)+mi]={4{32'hdead0000+mi}};expected[(32'h70000>>4)+mi]={4{32'hdead0000+mi}};
        end
        $readmemh({`CASE,".tex.hex"},tex_rom);
        for(upload=0;upload<4096;upload=upload+1) begin
            @(negedge clk);tap_sel=1;tap_en=0;tap_addr=upload*4;tap_data=tex_rom[upload/4][32*(upload%4) +:32];
            @(negedge clk);tap_en=1;@(negedge clk);tap_sel=0;tap_en=0;
        end
        expected_done=0;t_start=cycle;
        for(strip=0;strip<96/STRIP_H;strip=strip+1) begin
            strip_load=load_cycles;strip_exec=exec_cycles;strip_flush=flush_cycles;
            @(negedge clk);band_y0=strip*STRIP_H;band_start=1;@(posedge clk);while(!band_start_ready) @(posedge clk);@(negedge clk);band_start=0;
            cy=0;while(!band_loaded && cy<100000) begin @(negedge clk);cy=cy+1;end
            if(cy==100000) begin $display("LOAD_TIMEOUT");$display("RESULT: FAIL");$finish;end
            for(ci=0;ci<96;ci=ci+1) begin
                if(cmd_rom[ci][7:0]==8'hff) ci=96;
                else begin
                    has_pixels=$signed(cmd_rom[ci][95:80])<strip*STRIP_H+STRIP_H && $signed(cmd_rom[ci][95:80])+cmd_rom[ci][127:112]>strip*STRIP_H;
                    ci_geom_before=geom_done;ci_write_before=done_count;submit(ci);cy=0;
                    if(has_pixels) begin
                        while(geom_busy && cy<50000) begin @(negedge clk);cy=cy+1;end
                    end else begin
                        while(geom_done==ci_geom_before && cy<50000) begin @(negedge clk);cy=cy+1;end
                    end
                    if(cy==50000) begin $display("GEOM_TIMEOUT");$display("RESULT: FAIL");$finish;end
                    if(has_pixels) begin expected_done=expected_done+1;while(done_count==ci_write_before && cy<100000) begin @(negedge clk);cy=cy+1;end end
                    if(cy>=100000) begin $display("CMD_TIMEOUT");$display("RESULT: FAIL");$finish;end
                end
            end
            @(negedge clk);band_flush=1;@(posedge clk);while(!band_flush_ready) @(posedge clk);@(negedge clk);band_flush=0;
            cy=0;while(!band_flushed && cy<100000) begin @(negedge clk);cy=cy+1;end
            if(cy==100000) begin $display("FLUSH_TIMEOUT");$display("RESULT: FAIL");$finish;end
            $display("PHASE strip=%0d height=%0d load=%0d exec=%0d flush=%0d",strip,STRIP_H,load_cycles-strip_load,exec_cycles-strip_exec,flush_cycles-strip_flush);
        end
        $display("PHASE TOTAL height=%0d load=%0d exec=%0d flush=%0d",STRIP_H,load_cycles,exec_cycles,flush_cycles);
        $display("BAND FORMAL cycles=%0d fragments=%0d rows=%0d AR=%0d AW=%0d fd=%0d/%0d early=%0d preload=%0d",cycle-t_start,done_count,row_in,ar_seen,aw_seen,fd_count,fd_stat_count,early_attempts,preload_seen);
`ifndef EMPTY_CASE
`ifndef NO_FETCH
        if(fd_count!=128 || fd_stat_count!=1) begin errors=errors+1;$display("FETCH_NOT_EXERCISED");end
`else
        if(preload_seen==0) begin errors=errors+1;$display("PRELOAD_NOT_EXERCISED");end
`endif
        if(early_attempts==0) begin errors=errors+1;$display("EARLY_NOT_EXERCISED");end
`endif
        repeat(20) @(negedge clk);
        if(row_free!==2'b11) begin errors=errors+1;$display("ROWS_NOT_FREE %b",row_free);end
        if(row_in!=row_out || remaining!=0) begin
            errors=errors+1;$display("ROW_COUNT in=%0d out=%0d remaining=%0d",row_in,row_out,remaining);
        end
        if(u_ram.wlast_err!=0) begin errors=errors+1;$display("WLAST_ERROR %0d",u_ram.wlast_err);end
        mem_bad=0;
        for(mi=0;mi<32768;mi=mi+1) if(u_ram.mem[mi]!==expected[mi]) begin
            mem_bad=mem_bad+1;
            if(mem_bad<=20) $display("MEM_MISMATCH addr=%08x got=%032x want=%032x",mi*16,u_ram.mem[mi],expected[mi]);
        end
        errors=errors+mem_bad;
        $display("PATH stats commands=%0d read=%0d write=%0d stalls=%0d mem_bad=%0d errors=%0d",
                 done_count,read_count,write_count,stalls,mem_bad,errors);
        if(errors==0 && done_count==expected_done)
            $display("RESULT: PASS (B1-B4, software executor golden)");
        else $display("RESULT: FAIL (B1-B4 integration)");
        $finish;
    end
endmodule
`default_nettype wire
