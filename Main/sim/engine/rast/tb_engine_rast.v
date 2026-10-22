// R2 全链：真实 engine_mover + 随机反压 AXI RAM；每带 LOAD/clear/RAST/FLUSH。
// 每带收尾先对拍 BAND+z，再检查 DDR 落盘；所有退出路径均有明确判据。
`timescale 1ns/1ps
`default_nettype none
module tb_engine_rast;
    reg clk=0, rst=1, cancel=0;
    always #5 clk=~clk;
    localparam [31:0] SUB_BASE=32'h00100000, TRI_BASE=32'h00800000;
    localparam [31:0] BG_BASE=32'h01800000, DST_BASE=32'h01c00000;
    integer errors=0, cycles=0, commits=0, refs=0, axi_reads=0, axi_writes=0,tableoff=0;
    wire [31:0] m_fd_addr; wire [11:0] m_fd_bytes; wire [3:0] m_fd_cidx;
    wire m_fd_valid,m_fd_ready,m_fs_valid,m_fs_last,m_fs_ready,m_st_valid,m_st_error;
    wire [127:0] m_fs_data, rp_wd_w;
    wire [15:0] m_fs_keep; wire [3:0] m_fs_cidx,m_st_cidx;
    reg strip_ack_r=0, lease=0, band_start=0, band_flush=0;
    reg manual_mode=0,manual_valid=0,manual_ready=1;
    reg [1:0] manual_kind=0;
    reg [3:0] manual_tag=0;
    reg [15:0] band_y=0;
    wire loaded,flushed,band_error,band_start_ready,band_flush_ready;
    reg z_seen=0,load_seen=0;
    reg [31:0] prng=32'h13579bdf;
    wire band_ras_ready;
    wire stall_pixel = prng[2:0] == 3'd0 || (cycles%257 < 96);
    assign ras_wr_ready=band_ras_ready && !stall_pixel;
    always @(posedge clk) begin
        cycles<=cycles+1;
        prng<={prng[30:0],prng[31]^prng[21]^prng[1]^prng[0]};
        if (strip_req) begin z_seen<=0; load_seen<=0; end
        if (z_clear_done) z_seen<=1;
        if (loaded) load_seen<=1;
        if (rp_valid) refs<=refs+1;
        if (ras_wr_valid && ras_wr_ready) commits<=commits+1;
        if (!rst && u_rast.zwr_en !== (ras_wr_valid && ras_wr_ready))
            $fatal(1,"atomic color/z commit");
        if (!rst && (rast_err_valid || rlst_error || band_error || mo_tmo_evt)) begin
            $display("ERROR rast=%b rlst=%b code=%0d idx=%0d tag=%h band=%b tmo=%b",
                rast_err_valid,rlst_error,rlst_error_code,rlst_error_index,rlst_error_tag,band_error,mo_tmo_evt);
            $fatal(1,"unexpected engine error");
        end
        if (cycles > 40000000) $fatal(1,"WATCHDOG state=%0d strip=%0d refs=%0d retires=%0d",u_rlst.state,cur_strip,refs,dbg_retire_cnt);
    end
    // ---------------------------------------------------------------- rlst
    reg         rlst_go = 1'b0;
    reg  [31:0] tri_base = TRI_BASE;
    reg  [31:0] sub_base = SUB_BASE;
    reg  [23:0] sub_bytes = 24'd0;
    reg  [15:0] tri_count = 16'd0;
    wire        rp_we, rp_valid, strip_req, strip_done, frame_done, rlst_error;
    wire [1:0]  rp_slot, rp_slot_v, rp_free;
    wire [4:0]  rp_wa, rp_strip, cur_strip;
    wire [15:0] rp_index, rlst_error_index, rp_ordinal;
    wire [3:0]  rlst_error_code;
    wire [31:0] rlst_error_tag;
    wire        rlst_busy;

    engine_rlst u_rlst (
        .clk(clk), .rst(rst), .cancel(cancel), .go(rlst_go),
        .tri_base(tri_base), .sub_base(sub_base), .sub_bytes(sub_bytes), .tri_count(tri_count),
        .fd_addr(m_fd_addr), .fd_bytes(m_fd_bytes), .fd_cidx(m_fd_cidx),
        .fd_raster(), .fd_valid(m_fd_valid), .fd_ready(m_fd_ready),
        .fs_data(m_fs_data), .fs_valid(m_fs_valid), .fs_last(m_fs_last),
        .fs_ready(m_fs_ready), .fs_keep(m_fs_keep), .fs_cidx(m_fs_cidx), .st_cidx(m_st_cidx),
        .st_valid(m_st_valid), .st_error(m_st_error),
        .rp_we(rp_we), .rp_slot(rp_slot), .rp_wa(rp_wa), .rp_wd(rp_wd_w),
        .rp_valid(rp_valid), .rp_slot_v(rp_slot_v), .rp_index(rp_index), .rp_strip(rp_strip), .rp_ordinal(rp_ordinal),
        .rp_free(rp_free),
        .strip_req(strip_req), .cur_strip(cur_strip), .strip_ack(strip_ack_r),
        .strip_done(strip_done), .frame_done(frame_done),
        .error(rlst_error), .error_code(rlst_error_code),
        .error_index(rlst_error_index), .error_tag(rlst_error_tag), .busy(rlst_busy));

    // ---------------------------------------------------------------- rast
    wire        ras_wr_valid; wire [13:0] ras_wr_pix; wire [31:0] ras_wr_data;
    wire        ras_wr_ready;
    wire        z_clear_done; reg z_clear = 1'b0;
    wire        rast_err_valid; wire [3:0] rast_err_code;
    wire [15:0] rast_err_index; wire [31:0] rast_err_tag;
    wire [31:0] dbg_retire_cnt;
    reg  [31:0] retire_base = 32'd0;
`ifdef GFX_HAS_RAST_TEX
    wire        samp_valid, samp_ready, samp_rvalid;
    wire [11:0] samp_word;
    wire [31:0] samp_rdata;
`endif

    engine_rast #(
`ifdef RAST_TEX_LAT2
        .TEX_LATENCY(2)
`else
        .TEX_LATENCY(1)
`endif
    ) u_rast (
        .clk(clk), .rst(rst), .cancel(cancel),
        .rp_we(rp_we), .rp_slot(rp_slot), .rp_wa(rp_wa), .rp_wd(rp_wd_w),
        .rp_valid(rp_valid), .rp_slot_v(rp_slot_v), .rp_index(rp_index), .rp_strip(rp_strip), .rp_ordinal(rp_ordinal),
        .rp_free(rp_free),
        .ras_wr_valid(ras_wr_valid), .ras_wr_pix(ras_wr_pix),
        .ras_wr_data(ras_wr_data), .ras_wr_ready(ras_wr_ready),
        .z_clear(z_clear), .z_clear_busy(), .z_clear_done(z_clear_done),
`ifdef GFX_HAS_RAST_TEX
        .samp_valid(samp_valid), .samp_ready(samp_ready),
        .samp_word(samp_word), .samp_rvalid(samp_rvalid), .samp_rdata(samp_rdata),
        .tex_width(13'd64), .tex_height(13'd64), .tex_slog2(4'd6),
        .ras_key(32'hff000000),
`endif
        .err_valid(rast_err_valid), .err_code(rast_err_code),
        .err_index(rast_err_index), .err_tag(rast_err_tag),
        .dbg_retire_cnt(dbg_retire_cnt));

`ifdef GFX_HAS_RAST_TEX
    engine_tex #(
`ifdef RAST_TEX_LAT2
        .READ_LATENCY(2)
`else
        .READ_LATENCY(1)
`endif
    ) u_tex (
        .clk(clk), .rst(rst),
        .paddr(14'd0), .psel(1'b0), .penable(1'b0), .pwrite(1'b0), .pwdata(32'd0),
        .prdata(), .pready(), .pslverr(),
        .req_addr(14'd0), .req_bytes(12'd0), .req_buf(2'd0), .req_cidx(4'd0),
        .req_valid(1'b0), .req_ready(),
        .rs_tdata(), .rs_tvalid(),
        .rs_tlast(), .rs_buf(),
        .stat_valid(), .stat_ready(1'b1),
        .stat_cidx(), .stat_buf(), .busy(),
        .samp_valid(samp_valid), .samp_ready(samp_ready),
        .samp_word(samp_word), .samp_rvalid(samp_rvalid), .samp_rdata(samp_rdata));
    // atlas 左上 2×2 与 R1 纹理同布局（word = v*64+u → 行 0：词 0/1，行 1：词 64/65）
`endif
    wire  mo_pw_ready;
    wire  mo_pw_data_ready;
    wire  mo_pw_status;
    wire  mo_pw_error;
    wire  mo_pb_rd_beat;
    wire  mo_pb_wr_beat;
    wire  mo_band_perf_rd;
    wire  mo_band_perf_wr;
    wire  mo_rd_ready;
    wire [13:0] mo_tx_addr;
    wire [11:0] mo_tx_bytes;
    wire [1:0] mo_tx_buf;
    wire [3:0] mo_tx_cidx;
    wire  mo_tx_valid;
    wire  mo_tx_stat_ready;
    wire  mo_fs_instance;
    wire  mo_rd_stat_instance;
    wire [127:0] mo_rs_tdata;
    wire [15:0] mo_rs_tkeep;
    wire  mo_rs_tlast;
    wire  mo_rs_tvalid;
    wire [1:0] mo_rs_buf;
    wire [127:0] mo_fs_tdata;
    wire [15:0] mo_fs_tkeep;
    wire  mo_fs_tlast;
    wire  mo_fs_tvalid;
    wire  mo_wr_ready;
    wire  mo_ws_tready;
    wire  mo_band_active;
    wire  mo_band_busy;
    wire [3:0] mo_band_error_code;
    wire  mo_rd_stat_valid;
    wire  mo_rd_stat_fetch;
    wire [1:0] mo_rd_stat_buf;
    wire  mo_wr_stat_valid;
    wire  mo_wr_stat_err;
    wire [3:0] mo_wr_stat_cidx;
    wire  mo_wr_stat_last_row;
    wire  mo_drain_busy;
    wire  mo_drain_done;
    wire  mo_tmo_evt;
    wire  mo_tmo_dir;
    wire [31:0] ax_araddr;
    wire [7:0] ax_arlen;
    wire [2:0] ax_arsize;
    wire [1:0] ax_arburst;
    wire  ax_arvalid;
    wire  ax_arready;
    wire [127:0] ax_rdata;
    wire [1:0] ax_rresp;
    wire  ax_rlast;
    wire  ax_rvalid;
    wire  ax_rready;
    wire [31:0] ax_awaddr;
    wire [7:0] ax_awlen;
    wire [2:0] ax_awsize;
    wire [1:0] ax_awburst;
    wire  ax_awvalid;
    wire  ax_awready;
    wire [127:0] ax_wdata;
    wire [15:0] ax_wstrb;
    wire  ax_wlast;
    wire  ax_wvalid;
    wire  ax_wready;
    wire [1:0] ax_bresp;
    wire  ax_bvalid;
    wire  ax_bready;
    engine_mover #(.ENABLE_BAND(1)) u_mover (
        .clk(clk),
        .rst(rst),
        .pw_addr('0),
        .pw_bytes('0),
        .pw_valid('0),
        .pw_ready(mo_pw_ready),
        .pw_data('0),
        .pw_data_valid('0),
        .pw_data_ready(mo_pw_data_ready),
        .pw_status(mo_pw_status),
        .pw_error(mo_pw_error),
        .pb_rd_beat(mo_pb_rd_beat),
        .pb_wr_beat(mo_pb_wr_beat),
        .band_perf_rd(mo_band_perf_rd),
        .band_perf_wr(mo_band_perf_wr),
        .rd_addr('0),
        .rd_bytes('0),
        .rd_buf('0),
        .rd_cidx('0),
        .rd_tex('0),
        .rd_band('0),
        .rd_valid('0),
        .rd_ready(mo_rd_ready),
        .tx_addr(mo_tx_addr),
        .tx_bytes(mo_tx_bytes),
        .tx_buf(mo_tx_buf),
        .tx_cidx(mo_tx_cidx),
        .tx_valid(mo_tx_valid),
        .tx_ready('0),
        .tx_rs_tdata('0),
        .tx_rs_tvalid('0),
        .tx_rs_tlast('0),
        .tx_rs_buf('0),
        .tx_stat_valid('0),
        .tx_stat_ready(mo_tx_stat_ready),
        .tx_stat_cidx('0),
        .tx_stat_buf('0),
        .tx_busy('0),
        .fd_instance(manual_mode && manual_kind==1),
        .fs_instance(mo_fs_instance),
        .rd_stat_instance(mo_rd_stat_instance),
        .fd_addr(manual_mode ? BG_BASE : m_fd_addr),
        .fd_bytes(manual_mode ? 12'd128 : m_fd_bytes),
        .fd_cidx(manual_mode ? manual_tag : m_fd_cidx),
        .fd_valid(manual_mode ? manual_valid : m_fd_valid),
        .fd_ready(m_fd_ready),
        .fd_raster(manual_mode ? manual_kind==2 : 1'b1),
        .fsr_tdata(m_fs_data),
        .fsr_tvalid(m_fs_valid),
        .fsr_tlast(m_fs_last),
        .fsr_tkeep(m_fs_keep),
        .fsr_tready(manual_mode ? manual_ready : m_fs_ready),
        .rd_stat_raster(m_st_valid),
        .ras_lease(lease),
        .ras_wr_pix(ras_wr_pix),
        .ras_wr_data(ras_wr_data),
        .ras_wr_valid(ras_wr_valid && !stall_pixel),
        .ras_wr_ready(band_ras_ready),
        .rs_tdata(mo_rs_tdata),
        .rs_tkeep(mo_rs_tkeep),
        .rs_tlast(mo_rs_tlast),
        .rs_tvalid(mo_rs_tvalid),
        .rs_tready(1'b1),
        .rs_buf(mo_rs_buf),
        .fs_tdata(mo_fs_tdata),
        .fs_tkeep(mo_fs_tkeep),
        .fs_tlast(mo_fs_tlast),
        .fs_tvalid(mo_fs_tvalid),
        .fs_tready(manual_mode ? manual_ready : 1'b1),
        .fs_cidx(m_fs_cidx),
        .wr_addr('0),
        .wr_bytes('0),
        .wr_cidx('0),
        .wr_last_row('0),
        .wr_band('0),
        .wr_valid('0),
        .wr_ready(mo_wr_ready),
        .ws_tdata('0),
        .ws_tkeep('0),
        .ws_tvalid('0),
        .ws_tready(mo_ws_tready),
        .band_start(band_start),
        .band_flush(band_flush),
        .band_start_ready(band_start_ready),
        .band_flush_ready(band_flush_ready),
        .band_bg_base(BG_BASE),
        .band_dst_base(DST_BASE),
        .band_bg_slog2(5'd12),
        .band_dst_slog2(5'd12),
        .band_y0(band_y),
        .band_frame_height(16'd480),
        .band_loaded(loaded),
        .band_flushed(flushed),
        .band_active(mo_band_active),
        .band_busy(mo_band_busy),
        .band_error(band_error),
        .band_error_code(mo_band_error_code),
        .rd_stat_valid(mo_rd_stat_valid),
        .rd_stat_err(m_st_error),
        .rd_stat_fetch(mo_rd_stat_fetch),
        .rd_stat_cidx(m_st_cidx),
        .rd_stat_buf(mo_rd_stat_buf),
        .wr_stat_valid(mo_wr_stat_valid),
        .wr_stat_err(mo_wr_stat_err),
        .wr_stat_cidx(mo_wr_stat_cidx),
        .wr_stat_last_row(mo_wr_stat_last_row),
        .drain_req(cancel),
        .drain_busy(mo_drain_busy),
        .drain_done(mo_drain_done),
        .tmo_limit(13'd4096),
        .err_clr('0),
        .tmo_evt(mo_tmo_evt),
        .tmo_dir(mo_tmo_dir),
        .m_axi_araddr(ax_araddr),
        .m_axi_arlen(ax_arlen),
        .m_axi_arsize(ax_arsize),
        .m_axi_arburst(ax_arburst),
        .m_axi_arvalid(ax_arvalid),
        .m_axi_arready(ax_arready),
        .m_axi_rdata(ax_rdata),
        .m_axi_rresp(ax_rresp),
        .m_axi_rlast(ax_rlast),
        .m_axi_rvalid(ax_rvalid),
        .m_axi_rready(ax_rready),
        .m_axi_awaddr(ax_awaddr),
        .m_axi_awlen(ax_awlen),
        .m_axi_awsize(ax_awsize),
        .m_axi_awburst(ax_awburst),
        .m_axi_awvalid(ax_awvalid),
        .m_axi_awready(ax_awready),
        .m_axi_wdata(ax_wdata),
        .m_axi_wstrb(ax_wstrb),
        .m_axi_wlast(ax_wlast),
        .m_axi_wvalid(ax_wvalid),
        .m_axi_wready(ax_wready),
        .m_axi_bresp(ax_bresp),
        .m_axi_bvalid(ax_bvalid),
        .m_axi_bready(ax_bready)
    );
    axi_ram_model #(.MEM_WORDS(2097152),.STRB_EN(1),.SEED(27),.STALL(3)) u_ram (
        .clk(clk),
        .rst_n(!rst),
        .awaddr(ax_awaddr),
        .awlen(ax_awlen),
        .awvalid(ax_awvalid),
        .awready(ax_awready),
        .wdata(ax_wdata),
        .wstrb(ax_wstrb),
        .wlast(ax_wlast),
        .wvalid(ax_wvalid),
        .wready(ax_wready),
        .bresp(ax_bresp),
        .bvalid(ax_bvalid),
        .bready(ax_bready),
        .araddr(ax_araddr),
        .arlen(ax_arlen),
        .arvalid(ax_arvalid),
        .arready(ax_arready),
        .rdata(ax_rdata),
        .rresp(ax_rresp),
        .rlast(ax_rlast),
        .rvalid(ax_rvalid),
        .rready(ax_rready),
        .hold_aw(1'b0),
        .hold_ar(1'b0)
    );
    always @(posedge clk) begin
        if (ax_arvalid && ax_arready) axi_reads<=axi_reads+1;
        if (ax_awvalid && ax_awready) axi_writes<=axi_writes+1;
        if (ax_arvalid && ax_arready && ({1'b0,ax_araddr[11:0]} + (({5'd0,ax_arlen}+13'd1)<<4) > 4096))
            $fatal(1,"AXI page boundary");
    end
    reg [127:0] packed_word;
    reg [63:0] expected [0:614399];
    reg [7:0] framebytes [0:524287];
    function [31:0] f32;
        input integer k;
        begin f32={framebytes[k+3],framebytes[k+2],framebytes[k+1],framebytes[k]}; end
    endfunction
    function [63:0] rev64;
        input [63:0] v; integer k;
        begin for(k=0;k<8;k=k+1) rev64[8*k+:8]=v[8*(7-k)+:8]; end
    endfunction
    function [15:0] get_z;
        input integer p;
        begin
            case(p&3)
                0:get_z=(p>>2)<2048 ? u_rast.g_zbank[0].mem_big[p>>2] : u_rast.g_zbank[0].mem_small[(p>>2)-2048];
                1:get_z=(p>>2)<2048 ? u_rast.g_zbank[1].mem_big[p>>2] : u_rast.g_zbank[1].mem_small[(p>>2)-2048];
                2:get_z=(p>>2)<2048 ? u_rast.g_zbank[2].mem_big[p>>2] : u_rast.g_zbank[2].mem_small[(p>>2)-2048];
                3:get_z=(p>>2)<2048 ? u_rast.g_zbank[3].mem_big[p>>2] : u_rast.g_zbank[3].mem_small[(p>>2)-2048];
            endcase
        end
    endfunction
    function [31:0] get_band;
        input integer pix;
        integer w;
        begin
            w = pix >> 2;
            case (pix & 3)
                0: get_band = (w < 2048) ? u_mover.g_band.u_band.ram_lane[0].mem_big[w]
                                         : u_mover.g_band.u_band.ram_lane[0].mem_small[w-2048];
                1: get_band = (w < 2048) ? u_mover.g_band.u_band.ram_lane[1].mem_big[w]
                                         : u_mover.g_band.u_band.ram_lane[1].mem_small[w-2048];
                2: get_band = (w < 2048) ? u_mover.g_band.u_band.ram_lane[2].mem_big[w]
                                         : u_mover.g_band.u_band.ram_lane[2].mem_small[w-2048];
                default: get_band = (w < 2048) ? u_mover.g_band.u_band.ram_lane[3].mem_big[w]
                                         : u_mover.g_band.u_band.ram_lane[3].mem_small[w-2048];
            endcase
        end
    endfunction

    integer fd,n,i,k,j,offset,cursor,fi,nframes,total,subsize,substart,fcnt,sx,p;
    integer expected_refs, expected_commits, frame_refs, refbase, commitbase;
    reg [1023:0] mainfile,framefile,expfile;
    reg [31:0] got, ec;
    reg [15:0] ez;
    task owner_case;
        input integer kind;
        integer count,age,held;
        reg status_seen;
        reg [3:0] expected_tag;
        reg beat_valid,beat_last;
        reg [127:0] beat_data;
        begin
            manual_mode=1;manual_kind=kind;manual_tag=kind+4;
            expected_tag=kind+4;manual_ready=1;
            @(negedge clk);manual_valid=1;
            while(!m_fd_ready)@(negedge clk);
            @(negedge clk);manual_valid=0;
            // 请求侧改类/tag，旧返回仍必须使用已受理的上下文。
            manual_kind=(kind+1)%3;manual_tag=15;
            count=0;age=0;held=0;status_seen=0;
            while(count<8 || !status_seen) begin
                manual_ready=count<7 || held>=32;
                @(posedge clk);
                beat_valid=kind==2 ? m_fs_valid : mo_fs_tvalid;
                beat_last=kind==2 ? m_fs_last : mo_fs_tlast;
                beat_data=kind==2 ? m_fs_data : mo_fs_tdata;
                if(m_fs_valid && mo_fs_tvalid) $fatal(1,"FD stream dual owner");
                if(beat_valid && manual_ready)begin
                    if(beat_data!=={4{32'h12345678}} || beat_last!==(count==7) ||
                        m_fs_cidx!==expected_tag || (kind!=2 && mo_fs_instance!==(kind==1)))
                        $fatal(1,"FD owner data/tag kind=%0d beat=%0d",kind,count);
                    count=count+1;
                end
                if(count==7 && held<32) begin
                    held=held+1;
                    if(m_fd_ready) $fatal(1,"owner released before final stream beat");
                end
                if(mo_rd_stat_valid)begin
                    if(m_st_cidx!==expected_tag || m_st_error || m_st_valid!==(kind==2) ||
                        mo_rd_stat_instance!==(kind==1)) $fatal(1,"FD status owner/tag");
                    status_seen=1;
                end
                age=age+1;if(age>2000)$fatal(1,"owner preflight incomplete");
                @(negedge clk);
            end
            wait(m_fd_ready);@(negedge clk);manual_mode=0;
            $display("OWNER PASS kind=%0d beats=%0d last-held=%0d",kind,count,held);
        end
    endtask
    initial begin
        if (!$value$plusargs("MAIN=%s",mainfile) || !$value$plusargs("FRAME=%s",framefile) ||
            !$value$plusargs("EXP=%s",expfile)) $fatal(1,"missing vectors");
        if (!$value$plusargs("TRIOFF=%d",tableoff)) tableoff=0;
        if (!$value$plusargs("COMMITS=%d",expected_commits)) $fatal(1,"missing commit count");
        fd=$fopen(mainfile,"rb"); if (!fd) $fatal(1,"main open");
        n=$fread(u_ram.mem,fd,(TRI_BASE+tableoff)>>4); $fclose(fd);
        for(i=0;i<n/16;i=i+1) begin
            packed_word=u_ram.mem[((TRI_BASE+tableoff)>>4)+i];
            for(k=0;k<16;k=k+1) u_ram.mem[((TRI_BASE+tableoff)>>4)+i][8*k+:8]=packed_word[8*(15-k)+:8];
        end
        fd=$fopen(framefile,"rb"); if (!fd) $fatal(1,"frame open");
        n=$fread(framebytes,fd); $fclose(fd);
        if (f32(0)!==32'h52325354) $fatal(1,"frame magic");
        total=f32(4); nframes=f32(8);
        fd=$fopen(expfile,"rb"); if (!fd) $fatal(1,"expected open");
        n=$fread(expected,fd); $fclose(fd);
        if (n!=nframes*30*10240*8) $fatal(1,"expected size");
        for(i=0;i<n/8;i=i+1) expected[i]=rev64(expected[i]);
        for(j=0;j<480;j=j+1) for(i=0;i<160;i=i+1)
            u_ram.mem[(BG_BASE>>4)+j*256+i]={4{32'h12345678}};
`ifdef GFX_HAS_RAST_TEX
        for(i=0;i<1024;i=i+1) begin
            u_tex.g_bank[0].mem[i]=0; u_tex.g_bank[1].mem[i]=0;
            u_tex.g_bank[2].mem[i]=0; u_tex.g_bank[3].mem[i]=0;
        end
        u_tex.g_bank[0].mem[0]=32'hff000000; u_tex.g_bank[1].mem[0]=32'hff80ff40;
        u_tex.g_bank[0].mem[16]=32'hff4080ff; u_tex.g_bank[1].mem[16]=32'hffffffff;
`endif
        repeat(20) @(negedge clk); rst=0;
        owner_case(0);owner_case(2);owner_case(1);owner_case(0);
        cursor=32; expected_refs=0;
        for(fi=0;fi<nframes;fi=fi+1) begin
            offset=f32(cursor); subsize=f32(cursor+4); substart=cursor+128;
            fcnt=fi==nframes-1 ? total-offset : f32(substart+subsize)-offset;
            for(i=0;i<(subsize+15)/16;i=i+1) begin
                packed_word=0;
                for(k=0;k<16;k=k+1) if(i*16+k<subsize)
                    packed_word[8*k+:8]=framebytes[substart+i*16+k];
                u_ram.mem[(SUB_BASE>>4)+i]=packed_word;
            end
            frame_refs=0;
            for(i=0;i<30;i=i+1) begin
                p=substart+f32(cursor+8+i*4);
                frame_refs=frame_refs+{framebytes[p+1],framebytes[p]};
            end
            expected_refs=expected_refs+frame_refs;
            tri_base=TRI_BASE+tableoff+offset*128; sub_bytes=subsize; tri_count=fcnt;
            refbase=refs; commitbase=commits;
            @(negedge clk); rlst_go=1; @(negedge clk); rlst_go=0;
            for(sx=0;sx<30;sx=sx+1) begin
                wait(strip_req); @(negedge clk);
                if(cur_strip!=sx || !band_start_ready) $fatal(1,"strip lifecycle");
                band_y=sx*16; band_start=1; z_clear=1;
                @(negedge clk); band_start=0; z_clear=0;
                wait(load_seen && z_seen); @(negedge clk); lease=1; strip_ack_r=1;
                wait(strip_done); @(negedge clk);
                if(rp_free!=2'b11 || u_rast.in_flight!=0) $fatal(1,"early strip completion");
                for(k=0;k<10240;k=k+1) begin
                    {ez,ec}=expected[(fi*30+sx)*10240+k];
                    if(get_band(k)!==ec || get_z(k)!==ez) begin
                        if(errors<5) $display("MISMATCH f=%0d strip=%0d pix=%0d got=%h/%h want=%h/%h",fi,sx,k,get_band(k),get_z(k),ec,ez);
                        errors=errors+1;
                    end
                end
                if(errors) $fatal(1,"BAND/z compare errors=%0d",errors);
                lease=0;
                @(negedge clk); if(!band_flush_ready) $fatal(1,"flush not ready"); band_flush=1;
                @(negedge clk); band_flush=0;
                wait(flushed); @(negedge clk);
                for(k=0;k<10240;k=k+1) begin
                    got=u_ram.mem[(DST_BASE>>4)+(sx*16+k/640)*256+(k%640)/4] >> (32*(k%4));
                    ec=expected[(fi*30+sx)*10240+k][31:0];
                    if(got!==ec) $fatal(1,"DDR flush mismatch strip=%0d pix=%0d",sx,k);
                end
                strip_ack_r=0;
            end
            wait(frame_done); @(negedge clk);
            if(refs-refbase!=frame_refs) $fatal(1,"reference count");
            $display("FRAME PASS frame=%0d records=%0d refs=%0d commits=%0d",fi,fcnt,frame_refs,commits-commitbase);
            cursor=substart+subsize;
        end
        if(u_ram.wlast_err!=0 || dbg_retire_cnt!=expected_refs || commits!=expected_commits)
            $fatal(1,"completion counters actual commits=%0d expected=%0d",commits,expected_commits);
        $display("RESULT: PASS records=%0d refs=%0d retired=%0d commits=%0d AXI_R=%0d AXI_W=%0d cycles=%0d",
            total,refs,dbg_retire_cnt,commits,axi_reads,axi_writes,cycles);
        $finish;
    end
endmodule
`default_nettype wire
