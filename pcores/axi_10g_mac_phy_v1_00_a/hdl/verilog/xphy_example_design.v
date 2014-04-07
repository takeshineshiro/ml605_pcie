//-----------------------------------------------------------------------------
// Title      : Example Design level wrapper
// Project    : 10GBASE-R
//-----------------------------------------------------------------------------
// File       : xphy_example_design.v
//-----------------------------------------------------------------------------
// Description: This file is a wrapper for the 10GBASE-KR core; it contains all 
// of the clock buffers required for implementing the block level
//-----------------------------------------------------------------------------
// (c) Copyright 2009 - 2012 Xilinx, Inc. All rights reserved.
//
// This file contains confidential and proprietary information
// of Xilinx, Inc. and is protected under U.S. and 
// international copyright and other intellectual property
// laws.
//
// DISCLAIMER
// This disclaimer is not a license and does not grant any
// rights to the materials distributed herewith. Except as
// otherwise provided in a valid license issued to you by
// Xilinx, and to the maximum extent permitted by applicable
// law: (1) THESE MATERIALS ARE MADE AVAILABLE "AS IS" AND
// WITH ALL FAULTS, AND XILINX HEREBY DISCLAIMS ALL WARRANTIES
// AND CONDITIONS, EXPRESS, IMPLIED, OR STATUTORY, INCLUDING
// BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY, NON-
// INFRINGEMENT, OR FITNESS FOR ANY PARTICULAR PURPOSE; and
// (2) Xilinx shall not be liable (whether in contract or tort,
// including negligence, or under any other theory of
// liability) for any loss or damage of any kind or nature
// related to, arising under or in connection with these
// materials, including for any direct, or any indirect,
// special, incidental, or consequential loss or damage
// (including loss of data, profits, goodwill, or any type of
// loss or damage suffered as a result of any action brought
// by a third party) even if such damage or loss was
// reasonably foreseeable or Xilinx had been advised of the
// possibility of the same.
//
// CRITICAL APPLICATIONS
// Xilinx products are not designed or intended to be fail-
// safe, or for use in any application requiring fail-safe
// performance, such as life-support or safety devices or
// systems, Class III medical devices, nuclear facilities,
// applications related to the deployment of airbags, or any
// other applications that could lead to death, personal
// injury, or severe property or environmental damage
// (individually and collectively, "Critical
// Applications"). Customer assumes the sole risk and
// liability of any use of Xilinx products in Critical
// Applications, subject only to applicable laws and
// regulations governing limitations on product liability.
//
// THIS COPYRIGHT NOTICE AND DISCLAIMER MUST BE RETAINED AS
// PART OF THIS FILE AT ALL TIMES.


module xphy_example_design
  (
  input           refclk_p,
  input           refclk_n,
  output          core_clk156_out,
  input           reset,
  input  [63 : 0] xgmii_txd,
  input  [7 : 0]  xgmii_txc,
  output reg [63 : 0] xgmii_rxd,
  output reg [7 : 0]  xgmii_rxc,
  output          xgmii_rx_clk,
  output          txp,
  output          txn,
  input           rxp,
  input           rxn,
  input           mdc,
  input           mdio_in,
  output reg      mdio_out,
  output reg      mdio_tri,
  input [4 : 0]   prtad,
  output [7:0]    core_status,
  output          resetdone,
  input           signal_detect,
  input           tx_fault,
  output          tx_disable,
  output          is_eval,
  input wire an_enable,
  input wire training_enable,
  input wire [20:0] training_addr,
  input wire training_rnw,
  input wire [15:0] training_wrdata,
  input wire training_ipif_cs,
  input wire training_drp_cs,
  output wire [15:0] training_rddata,
  output wire training_rdack,
  output wire training_wrack);

  // Signal declarations
  wire clk156;
  wire txclk156_mmcm0_locked;
  
  // Sync the global reset to the relevant clocks
  reg core_reset_tx;
  reg core_reset_rx;
  reg txreset322;
  reg rxreset322;
  reg dclk_reset;
  
  reg core_reset_tx_tmp;
  reg core_reset_rx_tmp;
  reg txreset322_tmp;
  reg rxreset322_tmp;
  reg dclk_reset_tmp;
  
  (* KEEP = "true" *)
  wire txclk322;
  wire rxclk322;
  wire dclk;
  
  wire tx_resetdone_int;
  wire rx_resetdone_int;
  reg [63:0] xgmii_txd_reg;
  reg [7:0] xgmii_txc_reg;
  reg [63:0] xgmii_txd_reg1;
  reg [7:0] xgmii_txc_reg1;
  wire [63 : 0] xgmii_rxd_int;
  wire [7 : 0]  xgmii_rxc_int;
  
  wire mdio_out_int;
  wire mdio_tri_int;

  assign resetdone = tx_resetdone_int && rx_resetdone_int;
  
  //synthesis attribute async_reg of core_reset_tx_tmp is "true";
  //synthesis attribute async_reg of core_reset_tx is "true";
  //synthesis attribute async_reg of core_reset_rx_tmp is "true";
  //synthesis attribute async_reg of core_reset_rx is "true";
  always @(posedge reset or posedge clk156)
  begin
    if(reset)
    begin
      core_reset_tx_tmp <= 1'b1;
      core_reset_tx <= 1'b1;
      core_reset_rx_tmp <= 1'b1;
      core_reset_rx <= 1'b1;
    end
    else
    begin
      // Hold core in reset until everything else is ready...
      core_reset_tx_tmp <= (!(tx_resetdone_int) || reset || 
                        tx_fault || !(signal_detect) );
      core_reset_tx <= core_reset_tx_tmp;
      core_reset_rx_tmp <= (!(rx_resetdone_int) || reset || 
                        tx_fault || !(signal_detect) );
      core_reset_rx <= core_reset_rx_tmp;
    end
  end     
    
  // Create the other synchronized resets from the core reset...
    
  //synthesis attribute async_reg of txreset322_tmp is "true";
  //synthesis attribute async_reg of txreset322 is "true";
  always @(posedge reset or posedge txclk322)
  begin
    if(reset)
    begin
      txreset322_tmp <= 1'b1;
      txreset322 <= 1'b1;
    end
    else
    begin
      txreset322_tmp <= core_reset_tx;
      txreset322 <= txreset322_tmp;
    end
  end
  
  //synthesis attribute async_reg of rxreset322_tmp is "true";
  //synthesis attribute async_reg of rxreset322 is "true";
  always @(posedge reset or posedge rxclk322)
  begin
    if(reset)
    begin
      rxreset322_tmp <= 1'b1;
      rxreset322 <= 1'b1;
    end
    else
    begin
      rxreset322_tmp <= core_reset_rx;
      rxreset322 <= rxreset322_tmp;
    end
  end
  
  //synthesis attribute async_reg of dclk_reset_tmp is "true";
  //synthesis attribute async_reg of dclk_reset is "true";
  always @(posedge reset or posedge dclk)
  begin
    if(reset)
    begin
      dclk_reset_tmp <= 1'b1;
      dclk_reset <= 1'b1;
    end
    else
    begin
      dclk_reset_tmp <= core_reset_rx;
      dclk_reset <= dclk_reset_tmp;
    end
  end  
 
  // Add a pipeline to the xmgii_tx inputs, to aid timing closure
  always @(posedge clk156)
  begin
    xgmii_txd_reg1 <= xgmii_txd; 
    xgmii_txc_reg1 <= xgmii_txc; 
    xgmii_txd_reg <= xgmii_txd_reg1; 
    xgmii_txc_reg <= xgmii_txc_reg1; 
  end
     

  // Add a pipeline to the xmgii_rx outputs, to aid timing closure
  always @(posedge clk156)
  begin
    xgmii_rxd <= xgmii_rxd_int; 
    xgmii_rxc <= xgmii_rxc_int; 
  end
     
  // Add a pipeline to the mdio outputs, to aid timing closure
  // This is safe because the mdio clock is running so slowly
  always @(posedge clk156)
  begin
    mdio_out <= mdio_out_int; 
    mdio_tri <= mdio_tri_int; 
  end
     
  // Instantiate the 10GBASE-KR Block Level

  xphy_block # (
      .EXAMPLE_SIM_GTRESET_SPEEDUP("TRUE") ) //Does not affect hardware
  ten_gig_eth_pcs_pma_block
    (
      .refclk_n(refclk_n),
      .refclk_p(refclk_p),
      .clk156(clk156),
      .txclk322(txclk322),
      .rxclk322(rxclk322),
      .dclk(dclk),
      .areset(reset),
      .reset(core_reset_tx),
      .rxreset322(rxreset322),
      .txreset322(txreset322),
      .dclk_reset(dclk_reset),
      .xgmii_txd(xgmii_txd_reg),
      .xgmii_txc(xgmii_txc_reg),
      .xgmii_rxd(xgmii_rxd_int),
      .xgmii_rxc(xgmii_rxc_int),
      .txp(txp),
      .txn(txn),
      .rxp(rxp),
      .rxn(rxn),
       .mdc(mdc),
       .mdio_in(mdio_in),
       .mdio_out(mdio_out_int),
       .mdio_tri(mdio_tri_int),
       .prtad(prtad),
      .core_status(core_status),
      .tx_resetdone(tx_resetdone_int),
      .rx_resetdone(rx_resetdone_int),
      .signal_detect(signal_detect),
      .tx_fault(tx_fault),
      .tx_disable(tx_disable),
      .is_eval(is_eval),
      .an_enable(an_enable),
      .training_enable(training_enable),
      .training_addr(training_addr),
      .training_rnw(training_rnw),
      .training_wrdata(training_wrdata),
      .training_ipif_cs(training_ipif_cs),
      .training_drp_cs(training_drp_cs),
      .training_rddata(training_rddata),
      .training_rdack(training_rdack),
      .training_wrack(training_wrack)
      );
 
  assign core_clk156_out = clk156;

  ODDR #(.DDR_CLK_EDGE("SAME_EDGE")) rx_clk_ddr(
    .Q(xgmii_rx_clk),
    .D1(1'b0),
    .D2(1'b1),
    .C(clk156),
    .CE(1'b1),
    .R(1'b0),
    .S(1'b0));


endmodule
