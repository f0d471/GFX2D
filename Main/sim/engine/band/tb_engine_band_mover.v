// 真实 mover/AXI 控制验证：4KB 拆段、超时不放弃、LOAD/FLUSH 排空、错误重试。
`timescale 1ns/1ps
`default_nettype none
module tb_engine_band_mover;
    reg clk=0,rst=1;always #5 clk=~clk;
    reg [31:0] rd_addr=0,wr_addr=0;reg [11:0] rd_beats=1,wr_beats=1;
    wire [11:0] rd_bytes=rd_beats<<4,wr_bytes=wr_beats<<4;
    reg [1:0] rd_buf=2'b10;reg [3:0] rd_cidx=3,wr_cidx=9;
    wire rd_tex=1'b0;reg rd_valid=0,wr_valid=0,wr_last_row=1;
    wire rd_ready,wr_ready;wire [127:0] rs_tdata;
    wire [15:0] rs_tkeep;wire rs_tlast,rs_tvalid;wire rs_tready=1'b1;wire [1:0] rs_buf;
    reg [127:0] ws_tdata=0;wire [15:0] ws_tkeep=16'hffff;reg ws_tvalid=0;wire ws_tready;
    reg bstart=0,bflush=0,drain_req=0,err_clr=0,hold_ar=0,hold_aw=0,hold_w=0;
    wire ram_wready;
    assign wready=ram_wready && !hold_w;
    reg [31:0] bgbase=32'h1ff0,dstbase=32'h41ff0;
    wire cfg_ready,fready,loaded,flushed,active,bbusy,berror;wire [3:0] bcode;
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
    engine_mover #(.ENABLE_BAND(1)) u_mover(
        .clk(clk),.rst(rst),
        .band_start_ready(cfg_ready),.band_flush_ready(fready),.band_loaded(loaded),.band_flushed(flushed),
        .band_active(active),.band_busy(bbusy),.band_error(berror),.band_error_code(bcode),
        .rd_addr(rd_addr),.rd_bytes(rd_bytes),.rd_buf(rd_buf),.rd_tex(rd_tex), .rd_band(1'b1), .wr_band(1'b1), .band_start(bstart), .band_flush(bflush),
        .band_bg_base(bgbase), .band_dst_base(dstbase), .band_bg_slog2(5'd12), .band_dst_slog2(5'd12),
        .band_y0(16'd16), .band_frame_height(16'd480),
        .rd_cidx(rd_cidx),.rd_valid(rd_valid),.rd_ready(rd_ready),
        .tx_addr(),.tx_bytes(),.tx_buf(),.tx_cidx(),.tx_valid(),
        .tx_ready(1'b1),.tx_busy(1'b0),
        .tx_rs_tdata(128'd0),.tx_rs_tvalid(1'b0),.tx_rs_tlast(1'b0),.tx_rs_buf(2'd0),
        .tx_stat_valid(1'b0),.tx_stat_cidx(4'd0),.tx_stat_buf(2'd0),
        .fd_addr(32'd0),.fd_bytes(12'd0),.fd_cidx(4'd0),
        .fd_valid(1'b0),.fd_ready(),
        .rs_tdata(rs_tdata),.rs_tkeep(rs_tkeep),.rs_tlast(rs_tlast),
        .rs_tvalid(rs_tvalid),.rs_tready(rs_tready),.rs_buf(rs_buf),
        .fs_tdata(),.fs_tkeep(),.fs_tlast(),.fs_tvalid(),
        .fs_tready(1'b1),.fs_cidx(),
        .wr_addr(wr_addr),.wr_bytes(wr_bytes),.wr_cidx(wr_cidx),
        .wr_last_row(wr_last_row),.wr_valid(wr_valid),.wr_ready(wr_ready),
        .ws_tdata(ws_tdata),.ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid),.ws_tready(ws_tready),
        .rd_stat_valid(rd_stat_valid),.rd_stat_err(rd_stat_err),
        .rd_stat_fetch(),
        .rd_stat_cidx(rd_stat_cidx),.rd_stat_buf(rd_stat_buf),
        .wr_stat_valid(wr_stat_valid),.wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx),.wr_stat_last_row(wr_stat_last_row),
        .drain_req(drain_req),.drain_busy(drain_busy),.drain_done(drain_done),
        .tmo_limit(13'd64),.err_clr(err_clr),.tmo_evt(tmo_evt),
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
    axi_ram_model #(.MEM_WORDS(32768),.STRB_EN(1),.STALL(3),
                    .ADDR_STALL_MAX(3)) u_ram(
`endif
        .clk(clk),.rst_n(!rst),
        .awaddr(awaddr),.awlen(awlen),.awvalid(awvalid),.awready(awready),
        .wdata(wdata),.wstrb(wstrb),.wlast(wlast),.wvalid(wvalid && !hold_w),
        .wready(ram_wready),.bresp(bresp),.bvalid(bvalid),.bready(bready),
        .araddr(araddr),.arlen(arlen),.arvalid(arvalid),.arready(arready),
        .rdata(rdata),.rresp(rresp),.rlast(rlast),.rvalid(rvalid),
        .rready(rready),.hold_aw(hold_aw),.hold_ar(hold_ar));


    reg [127:0] expected[0:32767];
    integer i,p,row,beat,cy,errors=0,read_stats=0,write_stats=0,tmo_count=0,raw_words=0,ar_total=0,aw_total=0;
    reg tmo_expected=0,last_tmo_dir=0;integer stage=0;
    always @(posedge clk) if(!rst) begin
        if(rd_stat_valid) read_stats=read_stats+1;
        if(wr_stat_valid) write_stats=write_stats+1;
        if(tmo_evt) begin
            tmo_count=tmo_count+1;last_tmo_dir=tmo_dir;
            if(!tmo_expected) begin errors=errors+1;$display("CONTROL_UNEXPECTED_TIMEOUT stage=%0d dir=%0d",stage,tmo_dir);end
        end
        if(u_mover.ws_core_fire && u_mover.wr_ctx_flush && !u_mover.wr_drain_help) raw_words=raw_words+1;
        if(arvalid && arready) ar_total=ar_total+1;
        if(awvalid && awready) aw_total=aw_total+1;
    end
    function [127:0] pattern;
        input integer a;
        integer q;
        begin for(q=0;q<4;q=q+1) pattern[q*32 +:32]=32'hc0800000|((a*7+q*13)&32'h007fffff);end
    endfunction
    task load_band;
        begin
            @(negedge clk);bstart=1;@(posedge clk);while(!cfg_ready) @(posedge clk);@(negedge clk);bstart=0;
            cy=0;while(!loaded && !berror && cy<100000) begin @(negedge clk);cy=cy+1;end
            if(!loaded || berror) begin errors=errors+1;$display("CONTROL_LOAD");end
        end
    endtask
    task start_flush;
        begin @(negedge clk);bflush=1;@(posedge clk);while(!fready) @(posedge clk);@(negedge clk);bflush=0;end
    endtask
    task cancel_job;
        begin
            @(negedge clk);drain_req=1;cy=0;
            while(!drain_done && cy<100000) begin @(negedge clk);cy=cy+1;end
            if(!drain_done || bbusy) begin errors=errors+1;$display("CONTROL_DRAIN");end
            drain_req=0;repeat(5) @(negedge clk);
        end
    endtask
    task compare_mem;
        input integer kind;
        integer j,bad;
        begin
            bad=0;
            for(j=0;j<32768;j=j+1) if(u_ram.mem[j]!==expected[j]) begin
                bad=bad+1;if(bad<4) $display("CONTROL_MEM kind=%0d addr=%08x",kind,j*16);
            end
            if(bad!=0) begin errors=errors+bad;if(kind==2) $display("DRAIN_MEMORY bad=%0d",bad);end
        end
    endtask
    initial begin
        #1;
        for(i=0;i<32768;i=i+1) begin u_ram.mem[i]=pattern(i);expected[i]=pattern(i);end
        for(row=0;row<16;row=row+1) for(beat=0;beat<160;beat=beat+1)
            expected[(dstbase>>4)+(16+row)*256+beat]=expected[(bgbase>>4)+(16+row)*256+beat];
        repeat(5) @(negedge clk);rst=0;stage=1;load_band();
        repeat(80) @(negedge clk);
        if(tmo_count!=0) begin errors=errors+1;$display("CONTROL_IDLE_TIMEOUT");end
        // 接受本地写后暂不供流：只报告超时，仍保留该事务以便恢复。
        stage=2;tmo_expected=1;wr_addr=dstbase+16*4096;wr_valid=1;
        @(posedge clk);while(!wr_ready) @(posedge clk);@(negedge clk);wr_valid=0;
        repeat(80) @(negedge clk);
        if(tmo_count!=1 || !last_tmo_dir || !u_mover.b_wbusy) begin errors=errors+1;$display("CONTROL_LOCAL_TIMEOUT");end
        ws_tdata={32'h80000000,32'h00224466,32'h80909090,32'h00112233};ws_tvalid=1;
        @(posedge clk);while(!ws_tready) @(posedge clk);@(negedge clk);ws_tvalid=0;
        cy=0;while(write_stats<1 && cy<1000) begin @(negedge clk);cy=cy+1;end
        if(wr_stat_err || wr_stat_cidx!=9) begin errors=errors+1;$display("CONTROL_LOCAL_STATUS");end
        i=(dstbase>>4)+16*256;expected[i][31:0]=32'h00112233;expected[i][95:64]=32'h00224466;
        err_clr=1;@(negedge clk);err_clr=0;tmo_expected=0;
        stage=3;start_flush();cy=0;while(!flushed && cy<100000) begin @(negedge clk);cy=cy+1;end
        if(!flushed || read_stats!=0 || write_stats!=1 || ar_total!=32 || aw_total!=32) begin errors=errors+1;$display("CONTROL_NORMAL");end
        compare_mem(1);$display("CONTROL normal AR=%0d AW=%0d local=%0d",ar_total,aw_total,write_stats);
        // LOAD 的 AR 被从机阻塞时取消，必须等待已接受的 DMA 事务完成。
        stage=4;hold_ar=1;tmo_expected=1;
        @(negedge clk);bstart=1;@(posedge clk);while(!cfg_ready) @(posedge clk);@(negedge clk);bstart=0;
        wait(arvalid);repeat(80) @(negedge clk);
        if(last_tmo_dir || !bbusy) begin errors=errors+1;$display("CONTROL_READ_TIMEOUT");end
        drain_req=1;repeat(10) @(negedge clk);
        if(drain_done || !bbusy) begin errors=errors+1;$display("CONTROL_EARLY_DRAIN");end
        hold_ar=0;cy=0;while(!drain_done && cy<100000) begin @(negedge clk);cy=cy+1;end
        if(!drain_done) begin errors=errors+1;$display("CONTROL_CANCEL_LOAD");end
        drain_req=0;tmo_expected=0;repeat(5) @(negedge clk);
        // 在原始 flush 中取消：已排入 FIFO 的数据保留，剩余补拍必须零 strobe。
        stage=5;bgbase=32'h1000;dstbase=32'h40000;load_band();
        for(row=0;row<16;row=row+1) for(beat=0;beat<160;beat=beat+1) begin
            i=(dstbase>>4)+(16+row)*256+beat;u_ram.mem[i]=128'h5a5a5a5a;expected[i]=128'h5a5a5a5a;
        end
        stage=6;raw_words=0;hold_w=1;start_flush();
        cy=0;while(raw_words<10 && cy<5000) begin @(negedge clk);cy=cy+1;end
        if(raw_words<10) begin $display("RAW_PREFIX_NOT_READY phase=%0d rd=%0d wr=%0d",u_mover.g_band.u_band.phase,u_mover.rd_busy,u_mover.wr_busy);$display("RESULT: FAIL");$finish;end
        drain_req=1;repeat(5) @(negedge clk);hold_w=0;
        for(beat=0;beat<raw_words;beat=beat+1) expected[(dstbase>>4)+16*256+beat]=u_ram.mem[(bgbase>>4)+16*256+beat];
        cy=0;while(!drain_done && cy<100000) begin @(negedge clk);cy=cy+1;end
        if(!drain_done) begin errors=errors+1;$display("CONTROL_CANCEL_FLUSH");end
        drain_req=0;repeat(5) @(negedge clk);compare_mem(2);
        $display("CONTROL cancel raw-prefix=%0d WL=%0d",raw_words,u_ram.wlast_err);
        // 非 OKAY 读响应：不发布 loaded；恢复从机后能重试。
        stage=7;u_ram.err_lo_r=bgbase+16*4096;u_ram.err_hi_r=u_ram.err_lo_r+16;
        @(negedge clk);bstart=1;@(posedge clk);while(!cfg_ready) @(posedge clk);@(negedge clk);bstart=0;
        cy=0;while(!berror && cy<100000) begin @(negedge clk);cy=cy+1;end
        if(!berror || bcode!=3 || loaded) begin errors=errors+1;$display("CONTROL_READ_ERROR");end
        cancel_job();u_ram.err_lo_r=32'hffffffff;u_ram.err_hi_r=0;
        load_band();
        // 非 OKAY 写响应：状态返回错误，随后仍能重新提交。
        stage=8;u_ram.err_lo_r=dstbase+16*4096;u_ram.err_hi_r=u_ram.err_lo_r+16;
        start_flush();cy=0;while(!berror && cy<100000) begin @(negedge clk);cy=cy+1;end
        if(!berror || bcode!=4 || flushed) begin errors=errors+1;$display("CONTROL_WRITE_ERROR");end
        cancel_job();u_ram.err_lo_r=32'hffffffff;u_ram.err_hi_r=0;load_band();
        cancel_job();
        if(u_ram.wlast_err!=0 || errors!=0) $display("RESULT: FAIL");else $display("RESULT: PASS");
        $display("CONTROL errors=%0d timeouts=%0d read-status=%0d write-status=%0d",errors,tmo_count,read_stats,write_stats);
        $finish;
    end
    initial begin #30000000;$display("WATCHDOG stage=%0d phase=%0d rd=%0d wr=%0d drain=%0d hold=%0d/%0d raw=%0d",stage,u_mover.g_band.u_band.phase,u_mover.rd_busy,u_mover.wr_busy,drain_req,hold_ar,hold_aw,raw_words);$finish;end
endmodule
`default_nettype wire
