edb_top edb_top_inst (
    .bscan_CAPTURE      ( jtag_inst1_CAPTURE ),
    .bscan_DRCK         ( jtag_inst1_DRCK ),
    .bscan_RESET        ( jtag_inst1_RESET ),
    .bscan_RUNTEST      ( jtag_inst1_RUNTEST ),
    .bscan_SEL          ( jtag_inst1_SEL ),
    .bscan_SHIFT        ( jtag_inst1_SHIFT ),
    .bscan_TCK          ( jtag_inst1_TCK ),
    .bscan_TDI          ( jtag_inst1_TDI ),
    .bscan_TMS          ( jtag_inst1_TMS ),
    .bscan_UPDATE       ( jtag_inst1_UPDATE ),
    .bscan_TDO          ( jtag_inst1_TDO ),
    .mem_clk        ( $INSERT_YOUR_CLOCK_NAME ),
    .mem_error      ( mem_error ),
    .mem_err_cnt    ( mem_err_cnt ),
    .mem_err_addr   ( mem_err_addr ),
    .mem_err_position( mem_err_position ),
    .mem_ref_data   ( mem_ref_data ),
    .mem_err_data   ( mem_err_data ),
    .mem_ddr_reset_n( mem_ddr_reset_n ),
    .mem_mode_en    ( mem_mode_en ),
    .mem_axi_mode   ( mem_axi_mode ),
    .mem_addr_mode  ( mem_addr_mode ),
    .mem_len_mode   ( mem_len_mode ),
    .mem_data_mode  ( mem_data_mode ),
    .mem_saddr      ( mem_saddr ),
    .mem_faddr      ( mem_faddr ),
    .mem_flen       ( mem_flen ),
    .mem_fdata      ( mem_fdata ),
    .phy_debug_clk          ( $INSERT_YOUR_CLOCK_NAME ),
    .phy_debug_idelay_ld        ( phy_debug_idelay_ld ),
    .phy_debug_mpr_rdlvl_dly        ( phy_debug_mpr_rdlvl_dly ),
    .phy_debug_wrlvl_dq_check       ( phy_debug_wrlvl_dq_check ),
    .phy_debug_rd_level_dqs_check       ( phy_debug_rd_level_dqs_check ),
    .phy_debug_pll_shift_ena        ( phy_debug_pll_shift_ena ),
    .phy_debug_pll_shift_sel        ( phy_debug_pll_shift_sel ),
    .phy_debug_pll_shift        ( phy_debug_pll_shift ),
    .phy_debug_ddr_cs_n     ( phy_debug_ddr_cs_n ),
    .phy_debug_ddr_ras_n        ( phy_debug_ddr_ras_n ),
    .phy_debug_ddr_cas_n        ( phy_debug_ddr_cas_n ),
    .phy_debug_ddr_we_n     ( phy_debug_ddr_we_n ),
    .phy_debug_ddr_addr     ( phy_debug_ddr_addr ),
    .phy_debug_ddr_ba       ( phy_debug_ddr_ba ),
    .phy_debug_ddr_dqs_oe       ( phy_debug_ddr_dqs_oe ),
    .phy_debug_ddr_dq_oe        ( phy_debug_ddr_dq_oe ),
    .phy_debug_ddr_dqs_in_hi        ( phy_debug_ddr_dqs_in_hi ),
    .phy_debug_ddr_dqs_in_lo        ( phy_debug_ddr_dqs_in_lo ),
    .phy_debug_ddr_dq_in_hi     ( phy_debug_ddr_dq_in_hi ),
    .phy_debug_ddr_dq_in_lo     ( phy_debug_ddr_dq_in_lo ),
    .phy_debug_ddr_dqs_out_hi       ( phy_debug_ddr_dqs_out_hi ),
    .phy_debug_ddr_dqs_out_lo       ( phy_debug_ddr_dqs_out_lo ),
    .phy_debug_ddr_dq_out_hi        ( phy_debug_ddr_dq_out_hi ),
    .phy_debug_ddr_dq_out_lo        ( phy_debug_ddr_dq_out_lo ),
    .phy_debug_error        ( phy_debug_error ),
    .phy_debug_init_cur_state       ( phy_debug_init_cur_state ),
    .phy_debug_rdlvl_shift      ( phy_debug_rdlvl_shift ),
    .phy_debug_wrlvl_shift      ( phy_debug_wrlvl_shift ),
    .la0_clk            ( $INSERT_YOUR_CLOCK_NAME ),
    .la0_s_axi_awvalid      ( la0_s_axi_awvalid ),
    .la0_s_axi_awready      ( la0_s_axi_awready ),
    .la0_s_axi_awaddr       ( la0_s_axi_awaddr ),
    .la0_s_axi_awlen        ( la0_s_axi_awlen ),
    .la0_s_axi_awsize       ( la0_s_axi_awsize ),
    .la0_s_axi_wready       ( la0_s_axi_wready ),
    .la0_s_axi_wvalid       ( la0_s_axi_wvalid ),
    .la0_s_axi_wdata        ( la0_s_axi_wdata ),
    .la0_s_axi_wlast        ( la0_s_axi_wlast ),
    .la0_s_axi_bvalid       ( la0_s_axi_bvalid ),
    .la0_s_axi_bready       ( la0_s_axi_bready ),
    .la0_s_axi_bresp        ( la0_s_axi_bresp ),
    .la0_s_axi_arvalid      ( la0_s_axi_arvalid ),
    .la0_s_axi_arready      ( la0_s_axi_arready ),
    .la0_s_axi_araddr       ( la0_s_axi_araddr ),
    .la0_s_axi_arlen        ( la0_s_axi_arlen ),
    .la0_s_axi_rready       ( la0_s_axi_rready ),
    .la0_s_axi_rvalid       ( la0_s_axi_rvalid ),
    .la0_s_axi_rdata        ( la0_s_axi_rdata ),
    .la0_s_axi_rresp        ( la0_s_axi_rresp ),
    .la0_s_axi_rlast        ( la0_s_axi_rlast ),
    .la0_error      ( la0_error ),
    .la0_ddr_debug_port     ( la0_ddr_debug_port ),
    .la0_err_cnt        ( la0_err_cnt ),
    .la0_err_addr       ( la0_err_addr ),
    .la0_err_position       ( la0_err_position ),
    .la0_ref_data       ( la0_ref_data ),
    .la0_err_data       ( la0_err_data ),
    .mem1_clk       ( $INSERT_YOUR_CLOCK_NAME ),
    .mem1_TestTime  ( mem1_TestTime ),
    .mem1_OpTotCyc  ( mem1_OpTotCyc ),
    .mem1_OpActCyc  ( mem1_OpActCyc ),
    .mem1_OpEffic   ( mem1_OpEffic ),
    .mem1_RdOpEffic ( mem1_RdOpEffic ),
    .mem1_WrOpEffic ( mem1_WrOpEffic ),
    .mem1_WrPeriMin ( mem1_WrPeriMin ),
    .mem1_WrPeriAvg ( mem1_WrPeriAvg ),
    .mem1_WrPeriMax ( mem1_WrPeriMax ),
    .mem1_RdPeriMin ( mem1_RdPeriMin ),
    .mem1_RdPeriAvg ( mem1_RdPeriAvg ),
    .mem1_RdPeriMax ( mem1_RdPeriMax ),
    .mem1_TimeOut   ( mem1_TimeOut ),
    .mem1_cal_done  ( mem1_cal_done ),
    .mem1_StatiClr  ( mem1_StatiClr )
);

////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2013-2021 Efinix Inc. All rights reserved.              
//
// This   document  contains  proprietary information  which   is        
// protected by  copyright. All rights  are reserved.  This notice       
// refers to original work by Efinix, Inc. which may be derivitive       
// of other work distributed under license of the authors.  In the       
// case of derivative work, nothing in this notice overrides the         
// original author's license agreement.  Where applicable, the           
// original license agreement is included in it's original               
// unmodified form immediately below this header.                        
//                                                                       
// WARRANTY DISCLAIMER.                                                  
//     THE  DESIGN, CODE, OR INFORMATION ARE PROVIDED “AS IS” AND        
//     EFINIX MAKES NO WARRANTIES, EXPRESS OR IMPLIED WITH               
//     RESPECT THERETO, AND EXPRESSLY DISCLAIMS ANY IMPLIED WARRANTIES,  
//     INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF          
//     MERCHANTABILITY, NON-INFRINGEMENT AND FITNESS FOR A PARTICULAR    
//     PURPOSE.  SOME STATES DO NOT ALLOW EXCLUSIONS OF AN IMPLIED       
//     WARRANTY, SO THIS DISCLAIMER MAY NOT APPLY TO LICENSEE.           
//                                                                       
// LIMITATION OF LIABILITY.                                              
//     NOTWITHSTANDING ANYTHING TO THE CONTRARY, EXCEPT FOR BODILY       
//     INJURY, EFINIX SHALL NOT BE LIABLE WITH RESPECT TO ANY SUBJECT    
//     MATTER OF THIS AGREEMENT UNDER TORT, CONTRACT, STRICT LIABILITY   
//     OR ANY OTHER LEGAL OR EQUITABLE THEORY (I) FOR ANY INDIRECT,      
//     SPECIAL, INCIDENTAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES OF ANY    
//     CHARACTER INCLUDING, WITHOUT LIMITATION, DAMAGES FOR LOSS OF      
//     GOODWILL, DATA OR PROFIT, WORK STOPPAGE, OR COMPUTER FAILURE OR   
//     MALFUNCTION, OR IN ANY EVENT (II) FOR ANY AMOUNT IN EXCESS, IN    
//     THE AGGREGATE, OF THE FEE PAID BY LICENSEE TO EFINIX HEREUNDER    
//     (OR, IF THE FEE HAS BEEN WAIVED, $100), EVEN IF EFINIX SHALL HAVE 
//     BEEN INFORMED OF THE POSSIBILITY OF SUCH DAMAGES.  SOME STATES DO 
//     NOT ALLOW THE EXCLUSION OR LIMITATION OF INCIDENTAL OR            
//     CONSEQUENTIAL DAMAGES, SO THIS LIMITATION AND EXCLUSION MAY NOT   
//     APPLY TO LICENSEE.                                                
//
////////////////////////////////////////////////////////////////////////////////
