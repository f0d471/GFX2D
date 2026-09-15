------------- Begin Cut here for COMPONENT Declaration ------
component edb_top
  port (
         bscan_CAPTURE : in  std_logic;
         bscan_DRCK    : in  std_logic;
         bscan_RESET   : in  std_logic;
         bscan_RUNTEST : in  std_logic;
         bscan_SEL     : in  std_logic;
         bscan_SHIFT   : in  std_logic;
         bscan_TCK     : in  std_logic;
         bscan_TDI     : in  std_logic;
         bscan_TMS     : in  std_logic;
         bscan_UPDATE  : in  std_logic;
         bscan_TDO     : out std_logic;
         mem_clk       : in  std_logic;
         mem_error     : in  std_logic;
         mem_err_cnt   : in  std_logic_vector(15 downto 0);
         mem_err_addr  : in  std_logic_vector(29 downto 0);
         mem_err_position : in  std_logic_vector(7 downto 0);
         mem_ref_data  : in  std_logic_vector(63 downto 0);
         mem_err_data  : in  std_logic_vector(63 downto 0);
         mem_ddr_reset_n : out std_logic;
         mem_mode_en   : out std_logic;
         mem_axi_mode  : out std_logic_vector(1 downto 0);
         mem_addr_mode : out std_logic_vector(1 downto 0);
         mem_len_mode  : out std_logic_vector(2 downto 0);
         mem_data_mode : out std_logic_vector(1 downto 0);
         mem_saddr     : out std_logic_vector(27 downto 0);
         mem_faddr     : out std_logic_vector(27 downto 0);
         mem_flen      : out std_logic_vector(7 downto 0);
         mem_fdata     : out std_logic_vector(63 downto 0);
         phy_debug_clk : in  std_logic;
         phy_debug_idelay_ld : in  std_logic;
         phy_debug_mpr_rdlvl_dly : in  std_logic;
         phy_debug_wrlvl_dq_check : in  std_logic_vector(7 downto 0);
         phy_debug_rd_level_dqs_check : in  std_logic_vector(7 downto 0);
         phy_debug_pll_shift_ena : in  std_logic;
         phy_debug_pll_shift_sel : in  std_logic_vector(4 downto 0);
         phy_debug_pll_shift : in  std_logic_vector(2 downto 0);
         phy_debug_ddr_cs_n : in  std_logic;
         phy_debug_ddr_ras_n : in  std_logic;
         phy_debug_ddr_cas_n : in  std_logic;
         phy_debug_ddr_we_n : in  std_logic;
         phy_debug_ddr_addr : in  std_logic_vector(15 downto 0);
         phy_debug_ddr_ba : in  std_logic_vector(2 downto 0);
         phy_debug_ddr_dqs_oe : in  std_logic;
         phy_debug_ddr_dq_oe : in  std_logic;
         phy_debug_ddr_dqs_in_hi : in  std_logic_vector(1 downto 0);
         phy_debug_ddr_dqs_in_lo : in  std_logic_vector(1 downto 0);
         phy_debug_ddr_dq_in_hi : in  std_logic_vector(15 downto 0);
         phy_debug_ddr_dq_in_lo : in  std_logic_vector(15 downto 0);
         phy_debug_ddr_dqs_out_hi : in  std_logic_vector(1 downto 0);
         phy_debug_ddr_dqs_out_lo : in  std_logic_vector(1 downto 0);
         phy_debug_ddr_dq_out_hi : in  std_logic_vector(15 downto 0);
         phy_debug_ddr_dq_out_lo : in  std_logic_vector(15 downto 0);
         phy_debug_error : in  std_logic;
         phy_debug_init_cur_state : in  std_logic_vector(6 downto 0);
         phy_debug_rdlvl_shift : in  std_logic_vector(2 downto 0);
         phy_debug_wrlvl_shift : in  std_logic_vector(2 downto 0);
         la0_clk       : in  std_logic;
         la0_s_axi_awvalid : in  std_logic;
         la0_s_axi_awready : in  std_logic;
         la0_s_axi_awaddr : in  std_logic_vector(31 downto 0);
         la0_s_axi_awlen : in  std_logic_vector(7 downto 0);
         la0_s_axi_awsize : in  std_logic_vector(2 downto 0);
         la0_s_axi_wready : in  std_logic;
         la0_s_axi_wvalid : in  std_logic;
         la0_s_axi_wdata : in  std_logic_vector(63 downto 0);
         la0_s_axi_wlast : in  std_logic;
         la0_s_axi_bvalid : in  std_logic;
         la0_s_axi_bready : in  std_logic;
         la0_s_axi_bresp : in  std_logic_vector(1 downto 0);
         la0_s_axi_arvalid : in  std_logic;
         la0_s_axi_arready : in  std_logic;
         la0_s_axi_araddr : in  std_logic_vector(31 downto 0);
         la0_s_axi_arlen : in  std_logic_vector(7 downto 0);
         la0_s_axi_rready : in  std_logic;
         la0_s_axi_rvalid : in  std_logic;
         la0_s_axi_rdata : in  std_logic_vector(63 downto 0);
         la0_s_axi_rresp : in  std_logic_vector(1 downto 0);
         la0_s_axi_rlast : in  std_logic;
         la0_error     : in  std_logic;
         la0_ddr_debug_port : in  std_logic_vector(35 downto 0);
         la0_err_cnt   : in  std_logic_vector(15 downto 0);
         la0_err_addr  : in  std_logic_vector(31 downto 0);
         la0_err_position : in  std_logic_vector(7 downto 0);
         la0_ref_data  : in  std_logic_vector(63 downto 0);
         la0_err_data  : in  std_logic_vector(63 downto 0);
         mem1_clk      : in  std_logic;
         mem1_TestTime : in  std_logic_vector(23 downto 0);
         mem1_OpTotCyc : in  std_logic_vector(47 downto 0);
         mem1_OpActCyc : in  std_logic_vector(47 downto 0);
         mem1_OpEffic  : in  std_logic_vector(9 downto 0);
         mem1_RdOpEffic : in  std_logic_vector(9 downto 0);
         mem1_WrOpEffic : in  std_logic_vector(9 downto 0);
         mem1_WrPeriMin : in  std_logic_vector(9 downto 0);
         mem1_WrPeriAvg : in  std_logic_vector(9 downto 0);
         mem1_WrPeriMax : in  std_logic_vector(9 downto 0);
         mem1_RdPeriMin : in  std_logic_vector(9 downto 0);
         mem1_RdPeriAvg : in  std_logic_vector(9 downto 0);
         mem1_RdPeriMax : in  std_logic_vector(9 downto 0);
         mem1_TimeOut  : in  std_logic;
         mem1_cal_done : in  std_logic;
         mem1_StatiClr : out std_logic
       );
end component ;
---------------------- End COMPONENT Declaration ------------

-- The following code must appear in the VHDL architecture
-- body. Substitute your own instance name and net names.

------------- Begin Cut here for INSTANTIATION Template -----
edb_top_inst : edb_top
port map (
           bscan_CAPTURE => jtag_inst1_CAPTURE,
           bscan_DRCK    => jtag_inst1_DRCK,
           bscan_RESET   => jtag_inst1_RESET,
           bscan_RUNTEST => jtag_inst1_RUNTEST,
           bscan_SEL     => jtag_inst1_SEL,
           bscan_SHIFT   => jtag_inst1_SHIFT,
           bscan_TCK     => jtag_inst1_TCK,
           bscan_TDI     => jtag_inst1_TDI,
           bscan_TMS     => jtag_inst1_TMS,
           bscan_UPDATE  => jtag_inst1_UPDATE,
           bscan_TDO     => jtag_inst1_TDO,
           mem_clk       => #INSERT_YOUR_CLOCK_NAME,
           mem_error     => mem_error,
           mem_err_cnt   => mem_err_cnt,
           mem_err_addr  => mem_err_addr,
           mem_err_position => mem_err_position,
           mem_ref_data  => mem_ref_data,
           mem_err_data  => mem_err_data,
           mem_ddr_reset_n => mem_ddr_reset_n,
           mem_mode_en   => mem_mode_en,
           mem_axi_mode  => mem_axi_mode,
           mem_addr_mode => mem_addr_mode,
           mem_len_mode  => mem_len_mode,
           mem_data_mode => mem_data_mode,
           mem_saddr     => mem_saddr,
           mem_faddr     => mem_faddr,
           mem_flen      => mem_flen,
           mem_fdata     => mem_fdata,
           phy_debug_clk        => #INSERT_YOUR_CLOCK_NAME,
           phy_debug_idelay_ld  => phy_debug_idelay_ld,
           phy_debug_mpr_rdlvl_dly  => phy_debug_mpr_rdlvl_dly,
           phy_debug_wrlvl_dq_check => phy_debug_wrlvl_dq_check,
           phy_debug_rd_level_dqs_check => phy_debug_rd_level_dqs_check,
           phy_debug_pll_shift_ena  => phy_debug_pll_shift_ena,
           phy_debug_pll_shift_sel  => phy_debug_pll_shift_sel,
           phy_debug_pll_shift  => phy_debug_pll_shift,
           phy_debug_ddr_cs_n   => phy_debug_ddr_cs_n,
           phy_debug_ddr_ras_n  => phy_debug_ddr_ras_n,
           phy_debug_ddr_cas_n  => phy_debug_ddr_cas_n,
           phy_debug_ddr_we_n   => phy_debug_ddr_we_n,
           phy_debug_ddr_addr   => phy_debug_ddr_addr,
           phy_debug_ddr_ba => phy_debug_ddr_ba,
           phy_debug_ddr_dqs_oe => phy_debug_ddr_dqs_oe,
           phy_debug_ddr_dq_oe  => phy_debug_ddr_dq_oe,
           phy_debug_ddr_dqs_in_hi  => phy_debug_ddr_dqs_in_hi,
           phy_debug_ddr_dqs_in_lo  => phy_debug_ddr_dqs_in_lo,
           phy_debug_ddr_dq_in_hi   => phy_debug_ddr_dq_in_hi,
           phy_debug_ddr_dq_in_lo   => phy_debug_ddr_dq_in_lo,
           phy_debug_ddr_dqs_out_hi => phy_debug_ddr_dqs_out_hi,
           phy_debug_ddr_dqs_out_lo => phy_debug_ddr_dqs_out_lo,
           phy_debug_ddr_dq_out_hi  => phy_debug_ddr_dq_out_hi,
           phy_debug_ddr_dq_out_lo  => phy_debug_ddr_dq_out_lo,
           phy_debug_error  => phy_debug_error,
           phy_debug_init_cur_state => phy_debug_init_cur_state,
           phy_debug_rdlvl_shift    => phy_debug_rdlvl_shift,
           phy_debug_wrlvl_shift    => phy_debug_wrlvl_shift,
           la0_clk      => #INSERT_YOUR_CLOCK_NAME,
           la0_s_axi_awvalid    => la0_s_axi_awvalid,
           la0_s_axi_awready    => la0_s_axi_awready,
           la0_s_axi_awaddr => la0_s_axi_awaddr,
           la0_s_axi_awlen  => la0_s_axi_awlen,
           la0_s_axi_awsize => la0_s_axi_awsize,
           la0_s_axi_wready => la0_s_axi_wready,
           la0_s_axi_wvalid => la0_s_axi_wvalid,
           la0_s_axi_wdata  => la0_s_axi_wdata,
           la0_s_axi_wlast  => la0_s_axi_wlast,
           la0_s_axi_bvalid => la0_s_axi_bvalid,
           la0_s_axi_bready => la0_s_axi_bready,
           la0_s_axi_bresp  => la0_s_axi_bresp,
           la0_s_axi_arvalid    => la0_s_axi_arvalid,
           la0_s_axi_arready    => la0_s_axi_arready,
           la0_s_axi_araddr => la0_s_axi_araddr,
           la0_s_axi_arlen  => la0_s_axi_arlen,
           la0_s_axi_rready => la0_s_axi_rready,
           la0_s_axi_rvalid => la0_s_axi_rvalid,
           la0_s_axi_rdata  => la0_s_axi_rdata,
           la0_s_axi_rresp  => la0_s_axi_rresp,
           la0_s_axi_rlast  => la0_s_axi_rlast,
           la0_error    => la0_error,
           la0_ddr_debug_port   => la0_ddr_debug_port,
           la0_err_cnt  => la0_err_cnt,
           la0_err_addr => la0_err_addr,
           la0_err_position => la0_err_position,
           la0_ref_data => la0_ref_data,
           la0_err_data => la0_err_data,
           mem1_clk      => #INSERT_YOUR_CLOCK_NAME,
           mem1_TestTime => mem1_TestTime,
           mem1_OpTotCyc => mem1_OpTotCyc,
           mem1_OpActCyc => mem1_OpActCyc,
           mem1_OpEffic  => mem1_OpEffic,
           mem1_RdOpEffic => mem1_RdOpEffic,
           mem1_WrOpEffic => mem1_WrOpEffic,
           mem1_WrPeriMin => mem1_WrPeriMin,
           mem1_WrPeriAvg => mem1_WrPeriAvg,
           mem1_WrPeriMax => mem1_WrPeriMax,
           mem1_RdPeriMin => mem1_RdPeriMin,
           mem1_RdPeriAvg => mem1_RdPeriAvg,
           mem1_RdPeriMax => mem1_RdPeriMax,
           mem1_TimeOut  => mem1_TimeOut,
           mem1_cal_done => mem1_cal_done,
           mem1_StatiClr => mem1_StatiClr
         );
------------------------ End INSTANTIATION Template ---------

--------------------------------------------------------------------------------
-- Copyright (C) 2013-2021 Efinix Inc. All rights reserved.              
--
-- This   document  contains  proprietary information  which   is        
-- protected by  copyright. All rights  are reserved.  This notice       
-- refers to original work by Efinix, Inc. which may be derivitive       
-- of other work distributed under license of the authors.  In the       
-- case of derivative work, nothing in this notice overrides the         
-- original author's license agreement.  Where applicable, the           
-- original license agreement is included in it's original               
-- unmodified form immediately below this header.                        
--                                                                       
-- WARRANTY DISCLAIMER.                                                  
--     THE  DESIGN, CODE, OR INFORMATION ARE PROVIDED “AS IS” AND        
--     EFINIX MAKES NO WARRANTIES, EXPRESS OR IMPLIED WITH               
--     RESPECT THERETO, AND EXPRESSLY DISCLAIMS ANY IMPLIED WARRANTIES,  
--     INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF          
--     MERCHANTABILITY, NON-INFRINGEMENT AND FITNESS FOR A PARTICULAR    
--     PURPOSE.  SOME STATES DO NOT ALLOW EXCLUSIONS OF AN IMPLIED       
--     WARRANTY, SO THIS DISCLAIMER MAY NOT APPLY TO LICENSEE.           
--                                                                       
-- LIMITATION OF LIABILITY.                                              
--     NOTWITHSTANDING ANYTHING TO THE CONTRARY, EXCEPT FOR BODILY       
--     INJURY, EFINIX SHALL NOT BE LIABLE WITH RESPECT TO ANY SUBJECT    
--     MATTER OF THIS AGREEMENT UNDER TORT, CONTRACT, STRICT LIABILITY   
--     OR ANY OTHER LEGAL OR EQUITABLE THEORY (I) FOR ANY INDIRECT,      
--     SPECIAL, INCIDENTAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES OF ANY    
--     CHARACTER INCLUDING, WITHOUT LIMITATION, DAMAGES FOR LOSS OF      
--     GOODWILL, DATA OR PROFIT, WORK STOPPAGE, OR COMPUTER FAILURE OR   
--     MALFUNCTION, OR IN ANY EVENT (II) FOR ANY AMOUNT IN EXCESS, IN    
--     THE AGGREGATE, OF THE FEE PAID BY LICENSEE TO EFINIX HEREUNDER    
--     (OR, IF THE FEE HAS BEEN WAIVED, $100), EVEN IF EFINIX SHALL HAVE 
--     BEEN INFORMED OF THE POSSIBILITY OF SUCH DAMAGES.  SOME STATES DO 
--     NOT ALLOW THE EXCLUSION OR LIMITATION OF INCIDENTAL OR            
--     CONSEQUENTIAL DAMAGES, SO THIS LIMITATION AND EXCLUSION MAY NOT   
--     APPLY TO LICENSEE.                                                
--
--------------------------------------------------------------------------------
