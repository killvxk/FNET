/**************************************************************************
*
* Copyright 2011-2018 by Andrey Butok. FNET Community.
* Copyright 2008-2010 by Andrey Butok. Freescale Semiconductor, Inc.
*
***************************************************************************
*
*  Licensed under the Apache License, Version 2.0 (the "License"); you may
*  not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*  http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
*  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
*
***************************************************************************
*
*  FEC module driver implementation.
*
***************************************************************************/

#include "fnet.h"
#if (FNET_MCF || FNET_MK || FNET_MPC) && (FNET_CFG_CPU_ETH0 ||FNET_CFG_CPU_ETH1)

#include "fnet_fec.h"

/* The checksum field MUST be cleared. If NOT => the packet will have wrong checksum.
 * This is workaround, in case the checksum is not cleared.
 * You can set it to 0 if you use only one ENET interface. */
#define FNET_FEC_HW_TX_PROTOCOL_CHECKSUM_FIX       (1)

#if FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM && FNET_FEC_HW_TX_PROTOCOL_CHECKSUM_FIX
    #include "stack/fnet_icmp4.h"
    #include "stack/fnet_udp.h"
    #include "stack/fnet_tcp.h"
#endif

/******************************************************************************
 * FEC module RX interrupt support:
 *               - 1 = is enabled (Default value).
 *               - 0 = is disabled. Only for debug needs.
 *                     fnet_fec_poll() should be called periodically.
 ******************************************************************************/
#ifndef FNET_CFG_FEC_INTERRUPT_ENABLE
    #define FNET_CFG_FEC_INTERRUPT_ENABLE   1
#endif

#define FNET_FEC_ALIGN_DIV(div, x)     ((fnet_uint32_t)((fnet_uint8_t *)(x) + ((div)-1U)) & (~((div)-1U)))

/************************************************************************
*     Function Prototypes
*************************************************************************/
static fnet_return_t fnet_fec_init(fnet_netif_t *netif);
static void fnet_fec_release(fnet_netif_t *netif);
static void fnet_fec_input(fnet_netif_t *netif);
static void fnet_fec_rx_buf_next( fnet_fec_if_t *fec_if);
static fnet_return_t fnet_fec_get_hw_addr(fnet_netif_t *netif, fnet_uint8_t *hw_addr);
static fnet_return_t fnet_fec_set_hw_addr(fnet_netif_t *netif, fnet_uint8_t *hw_addr);
static fnet_bool_t fnet_fec_is_connected(fnet_netif_t *netif);
static fnet_return_t fnet_fec_get_statistics(fnet_netif_t *netif, struct fnet_netif_statistics *statistics);

/* FEC rx frame interrup handler. */
static void fnet_fec_isr_rx_handler_top(void *cookie);
static void fnet_fec_isr_rx_handler_bottom(void *cookie);

static void fnet_fec_get_mac_addr(fnet_fec_if_t *fec_if, fnet_mac_addr_t *mac_addr);

static void fnet_fec_phy_discover_addr (fnet_fec_if_t *fec_if, fnet_uint8_t phy_addr_start);
void fnet_fec_debug_mii_print_regs(fnet_netif_t *netif) ;

#if FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM && FNET_FEC_HW_TX_PROTOCOL_CHECKSUM_FIX
    static void fnet_fec_checksum_clear(fnet_uint8_t *datagram, fnet_size_t datagram_size);
#endif
#if FNET_CFG_MULTICAST
    static fnet_uint32_t fnet_fec_crc_hash(fnet_mac_addr_t multicast_addr);
#endif

/************************************************************************
*     Global Data Structures
*************************************************************************/

/* FEC/ENET Module control data structures.*/
#if FNET_CFG_CPU_ETH0 /*eth0*/

    #if (FNET_CFG_COMP_UV || FNET_CFG_COMP_GNUC || FNET_CFG_COMP_GHS) && FNET_CFG_CPU_CACHE && !FNET_CFG_CPU_CACHE_INVALIDATE
        __attribute__((section(".uncacheable")))
    #endif

    fnet_fec_if_t fnet_fec0_if

    #if FNET_CFG_COMP_IAR  && FNET_CFG_CPU_CACHE && !FNET_CFG_CPU_CACHE_INVALIDATE
        @".uncacheable"
    #endif
    ;
#endif

#if FNET_CFG_CPU_ETH1 /*eth1*/

    #if (FNET_CFG_COMP_UV || FNET_CFG_COMP_GNUC || FNET_CFG_COMP_GHS) && FNET_CFG_CPU_CACHE && !FNET_CFG_CPU_CACHE_INVALIDATE
        __attribute__((section(".uncacheable")))
    #endif

    fnet_fec_if_t fnet_fec1_if

    #if FNET_CFG_COMP_IAR  && FNET_CFG_CPU_CACHE && !FNET_CFG_CPU_CACHE_INVALIDATE
        @".uncacheable"
    #endif
    ;
#endif

/*****************************************************************************
 * FEC network-interface general API structure.
 ******************************************************************************/
const fnet_netif_api_t fnet_fec_api =
{
    .netif_type = FNET_NETIF_TYPE_ETHERNET,             /* Data-link type. */
    .netif_hw_addr_size = sizeof(fnet_mac_addr_t),
    .netif_init = fnet_fec_init,                        /* Initialization function.*/
    .netif_release = fnet_fec_release,                  /* Shutdown function.*/
#if FNET_CFG_IP4
    .netif_output_ip4 = fnet_eth_output_ip4,            /* IPv4 Transmit function.*/
#endif
    .netif_change_addr_notify = fnet_eth_change_addr_notify,    /* Address change notification function.*/
    .netif_drain = fnet_eth_drain,                      /* Drain function.*/
    .netif_get_hw_addr = fnet_fec_get_hw_addr,
    .netif_set_hw_addr = fnet_fec_set_hw_addr,
    .netif_is_connected = fnet_fec_is_connected,
    .netif_get_statistics = fnet_fec_get_statistics
#if FNET_CFG_MULTICAST
#if FNET_CFG_IP4
    , .netif_multicast_join_ip4 = fnet_eth_multicast_join_ip4
    , .netif_multicast_leave_ip4 = fnet_eth_multicast_leave_ip4
#endif
#if FNET_CFG_IP6
    , .netif_multicast_join_ip6 = fnet_eth_multicast_join_ip6
    , .netif_multicast_leave_ip6 = fnet_eth_multicast_leave_ip6
#endif
#endif
#if FNET_CFG_IP6
    , .netif_output_ip6 = fnet_eth_output_ip6           /* IPv6 Transmit function.*/
#endif
};

/************************************************************************
* DESCRIPTION: FEC/ENET Ethernet module initialization.
*************************************************************************/
static fnet_return_t fnet_fec_init(fnet_netif_t *netif)
{
    fnet_fec_if_t   *fec_if;
    fnet_index_t    i;
    fnet_return_t   result;

    fec_if = (fnet_fec_if_t *)(((fnet_eth_if_t *)(netif->netif_prv))->eth_prv);

#if !FNET_CFG_CPU_ETH_MIB
    /* Clear statistics. */
    fnet_memset_zero(&fec_if->statistics, sizeof(struct fnet_netif_statistics));
#endif

    /* Set the base address of FEC registers and other parameters, based on MAC number.*/
    switch (((fnet_eth_if_t *)(netif->netif_prv))->eth_mac_number)
    {
#if FNET_CFG_CPU_ETH0
        case 0:
            fec_if->reg = (volatile fnet_fec_reg_t *)FNET_FEC0_BASE_ADDR;  /* Set FEC module base refister pointer.*/
            fec_if->reg_phy = (volatile fnet_fec_reg_t *)FNET_FEC0_BASE_ADDR;
            fec_if->vector_number = FNET_CFG_CPU_ETH0_VECTOR_NUMBER;       /* Set RX Frame interrupt number.*/
            fec_if->phy_addr = FNET_CFG_CPU_ETH0_PHY_ADDR;                 /* Set default PHY address */
            break;
#endif
#if FNET_CFG_CPU_ETH1
        case 1:
            fec_if->reg = (volatile fnet_fec_reg_t *)FNET_FEC1_BASE_ADDR;  /* Set FEC module base refister pointer.*/
            fec_if->reg_phy = (volatile fnet_fec_reg_t *)FNET_FEC0_BASE_ADDR;
            fec_if->vector_number = FNET_CFG_CPU_ETH1_VECTOR_NUMBER;       /* Set RX Frame interrupt number.*/
            fec_if->phy_addr = FNET_CFG_CPU_ETH1_PHY_ADDR;                 /* Set default PHY address */
            break;
#endif
        default:
            result = FNET_ERR;
            goto ERROR;
    }

#if FNET_CFG_CPU_ETH_IO_INIT
    fnet_eth_io_init();
#endif

    /* Reset FEC.*/
    fec_if->reg->ECR = FNET_FEC_ECR_RESET;            /* Reset the Ethernet controller.*/
    while ((fec_if->reg->ECR & FNET_FEC_ECR_RESET) != 0u)
    {}  /* Wait for the reset sequence to complete.*/

    /* Disable FEC interrupts.*/
    fec_if->reg->EIMR = 0U;

    /* Clear any pending interrupts.*/
    fec_if->reg->EIR = 0xFFFFFFFFU;

    /* ======== Ethernet buffers initialisation ===============*/
    fec_if->tx_buf_desc = (fnet_fec_buf_desc_t *)FNET_FEC_ALIGN_DIV(FNET_FEC_BUF_DESC_DIV, fec_if->tx_buf_desc_buf);
    fec_if->rx_buf_desc = (fnet_fec_buf_desc_t *)FNET_FEC_ALIGN_DIV(FNET_FEC_BUF_DESC_DIV, fec_if->rx_buf_desc_buf);


    fec_if->tx_buf_desc_cur = fec_if->tx_buf_desc;
    fec_if->rx_buf_desc_cur = fec_if->rx_buf_desc;

    /* Initialize Tx descriptor rings.*/
    for (i = 0U; i < FNET_FEC_TX_BUF_NUM; i++)
    {
        fec_if->tx_buf_desc[i].status = FNET_HTONS(FNET_FEC_TX_BD_L | FNET_FEC_TX_BD_TC);
        fec_if->tx_buf_desc[i].length = FNET_HTONS(0U);

        fec_if->tx_buf_desc[i].buf_ptr = (fnet_uint8_t *)fnet_htonl(FNET_FEC_ALIGN_DIV(FNET_FEC_TX_BUF_DIV, fec_if->tx_buf[i]));
    }

    fec_if->tx_buf_desc_num = FNET_FEC_TX_BUF_NUM;

    /* Initialize Rx descriptor rings.*/
    for (i = 0U; i < FNET_FEC_RX_BUF_NUM; i++)
    {
        fec_if->rx_buf_desc[i].status = FNET_HTONS(FNET_FEC_RX_BD_E);
        fec_if->rx_buf_desc[i].length = FNET_HTONS(0U);

        fec_if->rx_buf_desc[i].buf_ptr = (fnet_uint8_t *)fnet_htonl(FNET_FEC_ALIGN_DIV(FNET_FEC_RX_BUF_DIV, fec_if->rx_buf[i]));
    }

    fec_if->rx_buf_desc_num = FNET_FEC_RX_BUF_NUM;

    /* Set the Wrap bit on the last one in the ring.*/
    fec_if->tx_buf_desc[FNET_FEC_TX_BUF_NUM - 1U].status |= FNET_HTONS(FNET_FEC_TX_BD_W);
    fec_if->rx_buf_desc[FNET_FEC_RX_BUF_NUM - 1U].status |= FNET_HTONS(FNET_FEC_RX_BD_W);

    /*======== END of Ethernet buffers initialisation ========*/

    /* Install RX Frame interrupt handler.*/
    result = fnet_isr_vector_init(fec_if->vector_number, fnet_fec_isr_rx_handler_top, fnet_fec_isr_rx_handler_bottom, FNET_CFG_CPU_ETH_VECTOR_PRIORITY, netif);

    if( result == FNET_OK)
    {
        /* Reset all multicast (hash table registers).*/
        fec_if->reg->GAUR = 0U;
        fec_if->reg->GALR = 0U;

        fec_if->reg->IAUR = 0U;
        fec_if->reg->IALR = 0U;

        /* Set Receive Buffer Size.*/
        fec_if->reg->EMRBR = FNET_FEC_BUF_SIZE;

        /* Pointer to Receive descriptor ring. */
        /* ERDSR points to the start of the circular receive buffer descriptor
         * queue in external memory. This pointer must be 32-bit aligned; however,
         * it is recommended it be made 128-bit aligned (evenly divisible by 16).*/
        fec_if->reg->ERDSR = (fnet_uint32_t)fec_if->rx_buf_desc;

        /* Pointer to Transmit descriptor ring. */
        /* ETDSR provides a pointer to the start of the circular transmit buffer
         * descriptor queue in external memory.
         * This pointer must be 32-bit aligned; however, it is recommended it be
         * made 128-bit aligned (evenly divisible by 16).*/
        fec_if->reg->ETDSR = (fnet_uint32_t)fec_if->tx_buf_desc;

        /* Set the transceiver interface.*/
        fec_if->reg->RCR = FNET_FEC_RCR_MII_MODE | FNET_FEC_RCR_MAX_FL(FNET_FEC_BUF_SIZE)
#if FNET_CFG_CPU_ETH_RMII /*Kinetis, Modelo*/
                          /* Set the transceiver interface to RMII mode (for Kinetis).*/
                          | FNET_FEC_RCR_RMII_MODE
#if FNET_CFG_CPU_ETH_RMII_10T
                          | FNET_FEC_RCR_RMII_10T
#endif
#endif /* FNET_CFG_CPU_ETH_RMII */
#if FNET_CFG_CPU_S32R274
                          | FNET_FEC_RCR_RGMII_EN /* TBD: make it configurable */
#endif
#if FNET_CFG_CPU_ETH_PROMISCUOUS
                          | FNET_FEC_RCR_PROM /* Enable promiscuous mode.*/
#endif
#if !FNET_CFG_CPU_ETH_FULL_DUPLEX
                          | FNET_FEC_RCR_DRT  /* Enable Half duplex.*/
#endif
                          ;
        /* Configure the transmit block.*/
        fec_if->reg->TCR = 0U
#if FNET_CFG_CPU_ETH_FULL_DUPLEX
                          | FNET_FEC_TCR_FDEN /* Enable Full duplex.*/
#endif
                          ;
#if FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM || FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM || FNET_CFG_CPU_ETH_HW_RX_IP_CHECKSUM || FNET_CFG_CPU_ETH_HW_RX_PROTOCOL_CHECKSUM || FNET_CFG_CPU_ETH_HW_RX_MAC_ERR
        /* Transmit Accelerator Function Configuration. */
        fec_if->reg->TACC = 0
#if FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM
                           | FNET_FEC_TACC_IPCHK   /* Insert IP header checksum.*/
#endif
#if FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM
                           | FNET_FEC_TACC_PROCHK  /* Insert Protocol checksum.*/
#endif
                           ;

        /* Receive Accelerator Function Configuration. */
        fec_if->reg->RACC = 0
#if FNET_CFG_CPU_ETH_HW_RX_IP_CHECKSUM
                           | FNET_FEC_RACC_IPDIS   /* Discard of frames with wrong IPv4 header checksum.*/
#endif
#if FNET_CFG_CPU_ETH_HW_RX_PROTOCOL_CHECKSUM
                           | FNET_FEC_RACC_PRODIS  /* Discard of frames with wrong protocol checksum.*/
#endif
#if FNET_CFG_CPU_ETH_HW_RX_MAC_ERR
                           | FNET_FEC_RACC_LINEDIS /* Discard of frames with MAC layer errors.*/
#endif
                           | FNET_FEC_RACC_PADREM  /* Padding removal for short IP frames.*/
                           ;

#if FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM || FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM
        /* Transmit FIFO Watermark Register.
         * The TFWR[STRFWD] bit must be set to use the checksum feature.
         * In this case, the MAC starts to transmit data only when a complete
         * frame is stored in the transmit FIFO.*/
        fec_if->reg->TFWR = FNET_FEC_TFWR_STRFWD;
#endif

#if FNET_CFG_CPU_ETH_HW_RX_IP_CHECKSUM || FNET_CFG_CPU_ETH_HW_RX_PROTOCOL_CHECKSUM
        /* Discarding is only available when the RX FIFO operates in store and forward
          mode (RSFL cleared).*/
        fec_if->reg->RSFL = 0;
#endif

        /* Set network interface features, for upper layer.*/
        netif->features = 0
#if FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM
                          | FNET_NETIF_FEATURE_HW_TX_IP_CHECKSUM         /* TX IP header checksum feature.*/
#endif
#if FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM
                          | FNET_NETIF_FEATURE_HW_TX_PROTOCOL_CHECKSUM   /* RX Protocol checksum feature.*/
#endif
#if FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM
                          | FNET_NETIF_FEATURE_HW_RX_IP_CHECKSUM         /* RX IP header checksum feature.*/
#endif
#if FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM
                          | FNET_NETIF_FEATURE_HW_RX_PROTOCOL_CHECKSUM   /* RX Protocol checksum feature.*/
#endif
                          ;

#endif /* FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM || FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM || FNET_CFG_CPU_ETH_HW_RX_IP_CHECKSUM || FNET_CFG_CPU_ETH_HW_RX_PROTOCOL_CHECKSUM || FNET_CFG_CPU_ETH_HW_RX_MAC_ERR */

        /*The MII_SPEED field must be programmed with a value to provide
        * an EMDC frequency of less than or equal to 2.5 MHz to be compliant
        * with the IEEE 802.3 MII specification.*/
#if FNET_CFG_CPU_MPC564xBC || FNET_CFG_CPU_MPC5668G || FNET_CFG_CPU_MPC5566 || FNET_MCF /* Older platforms.*/
        /* MDC formula defined in the corresponding MCU Reference Manual looks like : (SYS_CLK / PREDIV) x (1 / MII_SPEED) = MDC */
        fec_if->reg_phy->MSCR = FNET_FEC_MSCR_MII_SPEED((FNET_FEC_CLOCK_KHZ / FNET_FEC_MII_CLOCK_KHZ) + (fnet_uint32_t)(((FNET_FEC_CLOCK_KHZ % FNET_FEC_MII_CLOCK_KHZ) != 0U) ? 1U : 0U));
#elif FNET_CFG_CPU_S32R274
        //fec_if->reg_phy->MSCR = FNET_FEC_MSCR_MII_SPEED((FNET_FEC_CLOCK_KHZ/FNET_FEC_MII_CLOCK_KHZ) - 1U) & ~1U;
        fec_if->reg_phy->MSCR = 0x16; //TBD make it result of calculation
#else  /* FNET_CFG_CPU_MPC5744P || FNET_CFG_CPU_MCF54418 || FNET_CFG_CPU_MK60N512 || FNET_CFG_CPU_MK60DN512 || FNET_CFG_CPU_MK64FN1 || FNET_CFG_CPU_MK66FN2 || FNET_CFG_CPU_MK70FN1 || FNET_CFG_CPU_MK60FN1 */
        /* MDC formula defined in the corresponding MCU Reference Manual looks like : (SYS_CLK / PREDIV) x (1 / ((MII_SPEED + 1)) = MDC */
        fec_if->reg_phy->MSCR = FNET_FEC_MSCR_MII_SPEED((FNET_FEC_CLOCK_KHZ / FNET_FEC_MII_CLOCK_KHZ) - (fnet_uint32_t)(((FNET_FEC_CLOCK_KHZ % FNET_FEC_MII_CLOCK_KHZ) != 0U) ? 0U : 1U));
#endif

        /* Enable interrupts (Receive frame interrupt).*/
#if FNET_CFG_FEC_INTERRUPT_ENABLE
        fec_if->reg->EIMR = FNET_FEC_EIMR_RXF;
#endif

        /* Enable FEC */
#if FNET_CFG_CPU_S32R274
        fec_if->reg->ECR = FNET_FEC_ECR_ETHER_EN | FNET_FEC_ECR_SPEED;
#else
        fec_if->reg->ECR = FNET_FEC_ECR_ETHER_EN;
#endif


#if FNET_CFG_CPU_ETH_PHY_ADDR_DISCOVER
        fnet_fec_phy_discover_addr(fec_if, fec_if->phy_addr);
#endif

        fnet_eth_phy_init(fec_if);

        /* Set to not valid zero MAC address.*/
        fec_if->reg->PALR = 0U;
        fec_if->reg->PAUR = 0U;

        /* Clear Ethernet statistics. */
#if FNET_CFG_CPU_ETH_MIB
        fec_if->reg->MIBC = FNET_FEC_MIBC_MIB_DISABLE; /* Disable MIB */
        fec_if->reg->RMON_T_PACKETS = 0U;
        fec_if->reg->RMON_R_PACKETS = 0U;
        fec_if->reg->MIBC &= ~FNET_FEC_MIBC_MIB_DISABLE; /* Enable MIB */
#endif
#if FNET_CFG_MULTICAST
        fec_if->GALR_double = 0U;
        fec_if->GAUR_double = 0U;
#endif /*FNET_CFG_MULTICAST*/

#if FNET_CFG_CPU_ETH_ATONEGOTIATION_TIMEOUT
        /* Wait for autonegotiation. */
        {
            fnet_uint16_t   status;
            fnet_time_t     last_time;

            if (fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_SR, &status) == FNET_ERR)
            {
                status = 0U;
            }

            if ((status & FNET_FEC_MII_REG_SR_AN_ABILITY) != 0u)
            {
                /* Has auto-negotiate ability. */
                last_time =  fnet_timer_get_ticks();

                do
                {
                    if ((status & FNET_FEC_MII_REG_SR_AN_COMPLETE) != 0u)
                    {
                        /* Autonegotiation is complete.*/
                        break;
                    }
                }
                while ( (fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_SR, &status) == FNET_OK)
                        && (  fnet_timer_get_interval(last_time, fnet_timer_get_ticks()) <= ((FNET_CFG_CPU_ETH_ATONEGOTIATION_TIMEOUT) / FNET_TIMER_PERIOD_MS) ) );
            }
        }
#endif /* FNET_CFG_CPU_ETH_ATONEGOTIATION_TIMEOUT */

        /* Indicate that there have been empty receive buffers produced.*/
        fec_if->reg->RDAR = FNET_FEC_RDAR_R_DES_ACTIVE;
    }
ERROR:
    return result;
}

/************************************************************************
* DESCRIPTION: FEC/ENET Ethernet module release.
*************************************************************************/
static void fnet_fec_release(fnet_netif_t *netif)
{
    fnet_fec_if_t *fec_if = (fnet_fec_if_t *)(((fnet_eth_if_t *)(netif->netif_prv))->eth_prv);

    /* Note: Sometimes it is not possible communicate with the Ethernet PHY,
     * all reads come back as 0xFFFF. It appears that the problem is
     * that we need to disable the MDC clock by writing 0 to MSCR
     * before disabling the FEC: */
    fec_if->reg->MSCR = 0U;           /* Disable the MDC clock. */

    fec_if->reg->ECR = 0U;            /* Disable the Ethernet FEC. */

    fec_if->reg->EIMR = 0x0U;         /* Disable FEC interrupts. */

    fec_if->reg->EIR = 0xFFFFFFFFU;   /* Clear any pending FEC interrupt flags. */

    fnet_isr_unregister(fec_if->vector_number);

    fnet_eth_release(netif); /* Common Ethernet-interface release.*/
}

/************************************************************************
* DESCRIPTION: Stop Ethernet module.
*              For debugging needs.
*************************************************************************/
void fnet_fec_stop(fnet_netif_t *netif)
{
    fnet_fec_if_t *fec_if = (fnet_fec_if_t *)(((fnet_eth_if_t *)(netif->netif_prv))->eth_prv);

    fec_if->reg->EIMR = 0x0U;  /* Disable FEC interrupts. */
}

/************************************************************************
* DESCRIPTION: Resume Ethernet module.
*              For debugging needs.
*************************************************************************/
void fnet_fec_resume(fnet_netif_t *netif)
{
    fnet_fec_if_t *fec_if = (fnet_fec_if_t *)(((fnet_eth_if_t *)(netif->netif_prv))->eth_prv);

    fec_if->reg->EIMR = FNET_FEC_EIMR_RXF;
}

/************************************************************************
* DESCRIPTION: Ethernet input function.
*************************************************************************/
static void fnet_fec_input(fnet_netif_t *netif)
{
    fnet_fec_if_t       *fec_if = (fnet_fec_if_t *)(((fnet_eth_if_t *)(netif->netif_prv))->eth_prv);

#if FNET_CFG_CPU_CACHE_INVALIDATE
    fnet_cpu_cache_invalidate();
#endif

    /* While buffer !(empty or rx in progress)*/
    while((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_E)) == 0u)
    {

#if !FNET_CFG_CPU_ETH_MIB
        fec_if->statistics.rx_packet++;
#endif

        /* If !(buffer is last in the frame) */
        if ((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_L)) == 0u)
        {
            /* Skip the splitted frame. */
            do /* Keep going until we find the last one. */
            {
                fnet_fec_rx_buf_next(fec_if);
            }
            while ((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_L)) == 0u);
        }
        else
        {
            /* Error handling */
            if ((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_LG /* Frame too long.*/
                    | FNET_FEC_RX_BD_SH /* Frame too short.*/
                    | FNET_FEC_RX_BD_NO /* Frame alignment.*/
                    | FNET_FEC_RX_BD_CR /* CRC Error.*/
                    | FNET_FEC_RX_BD_OV /* FIFO overrun.*/
                    | FNET_FEC_RX_BD_TR /* Frame is truncated.*/
                                                            )) != 0u )
            {
                goto NEXT_FRAME;
            }

            /* Ethernet input.*/
            fnet_eth_input( netif, (fnet_uint8_t *)fnet_ntohl((fnet_uint32_t)fec_if->rx_buf_desc_cur->buf_ptr), fnet_ntohs(fec_if->rx_buf_desc_cur->length));
        }
    NEXT_FRAME:
        fnet_fec_rx_buf_next(fec_if);
    } /* while */
}

/************************************************************************
* DESCRIPTION: Ethernet input function.
*   !!!! Used for debug needs only!!!!!
*************************************************************************/
fnet_size_t fnet_fec_input_frame(fnet_netif_t *netif, fnet_uint8_t *buf, fnet_size_t buf_size)
{
    fnet_fec_if_t       *fec_if = (fnet_fec_if_t *)(((fnet_eth_if_t *)(netif->netif_prv))->eth_prv);
    fnet_eth_header_t   *ethheader;
    fnet_size_t         result = 0u;

    fnet_mac_addr_t local_mac_addr;

    fnet_cpu_cache_invalidate();

    /* While buffer ! (empty or rx in progress)*/
    if((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_E)) == 0u )
    {

#if !FNET_CFG_CPU_ETH_MIB
        fec_if->statistics.rx_packet++;
#endif
        /* If !(buffer last in frame) */
        if ((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_L)) == 0u)
        {
            /* Skip the splitted frame. */
            do /* Keep going until we find the last one. */
            {
                fnet_fec_rx_buf_next(fec_if);
            }
            while ((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_L)) == 0u);
        }
        else
        {
            /* Error handling */
            if ((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_LG /* Frame too long.*/
                    | FNET_FEC_RX_BD_SH /* Frame too short.*/
                    | FNET_FEC_RX_BD_NO /* Frame alignment.*/
                    | FNET_FEC_RX_BD_CR /* CRC Error.*/
                    | FNET_FEC_RX_BD_OV /* FIFO overrun.*/
                    | FNET_FEC_RX_BD_TR /* Frame is truncated.*/
                                                            )) != 0u )
            {
                goto NEXT_FRAME;
            }

            /* Point to the ethernet header.*/
            ethheader = (fnet_eth_header_t *)fnet_ntohl((fnet_uint32_t)fec_if->rx_buf_desc_cur->buf_ptr);

            fnet_fec_get_mac_addr(fec_if, &local_mac_addr);
            if(!fnet_memcmp(ethheader->source_addr, local_mac_addr, sizeof(local_mac_addr)))
            {
                goto NEXT_FRAME;
            }

            if(fnet_ntohs(fec_if->rx_buf_desc_cur->length) < buf_size)
            {
                fnet_memcpy(buf, (const void *)ethheader, (fnet_size_t)fnet_ntohs(fec_if->rx_buf_desc_cur->length));
                result = (fnet_size_t)fnet_ntohs(fec_if->rx_buf_desc_cur->length);
            }

            fnet_eth_trace("\nRX", ethheader); /* Print ETH header.*/

        }
    NEXT_FRAME:
        fnet_fec_rx_buf_next(fec_if);
    }

    return result;
}

/************************************************************************
* DESCRIPTION: Goes not the next Rx buffer.
*************************************************************************/
static void fnet_fec_rx_buf_next( fnet_fec_if_t *fec_if)
{
    /* Mark the buffer as empty.*/
    fec_if->rx_buf_desc_cur->status &= FNET_HTONS(FNET_FEC_RX_BD_W);
    fec_if->rx_buf_desc_cur->status |= FNET_HTONS(FNET_FEC_RX_BD_E);

    /* Update pointer to next entry.*/
    if ((fec_if->rx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_W)) != 0u)
    {
        fec_if->rx_buf_desc_cur = fec_if->rx_buf_desc;
    }
    else
    {
        fec_if->rx_buf_desc_cur++;
    }

    /* Indicate that there have been empty receive buffers produced.*/
    fec_if->reg->RDAR = FNET_FEC_RDAR_R_DES_ACTIVE;
}

/************************************************************************
* DESCRIPTION: This is WORKAROUND.
* If an IP frame with a known protocol is transmitted, the checksum
* is inserted automatically into the frame.
* !!!! The checksum field MUST be cleared, otherwise the checksum will be corrupted. !!!!
*************************************************************************/
#if FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM && FNET_FEC_HW_TX_PROTOCOL_CHECKSUM_FIX
static void fnet_fec_checksum_clear(fnet_uint8_t *datagram, fnet_size_t datagram_size)
{
    fnet_eth_header_t   *eth_hdr = (fnet_eth_header_t *)datagram;
    fnet_uint8_t        *ip_hdr = datagram+sizeof(fnet_eth_header_t);
    fnet_uint8_t        protocol = 0;
    fnet_size_t         ip_hdr_size;
    fnet_uint8_t        *prot_hdr;

    /* IPv4 */
    if((eth_hdr->type == FNET_HTONS(FNET_ETH_TYPE_IP4)) && (datagram_size >= sizeof(fnet_ip4_header_t)))
    {
        /* If NOT fragmented. The MF bit or fragment offset are zero.*/
        if((((fnet_ip4_header_t *)ip_hdr)->flags_fragment_offset & ~FNET_HTONS(FNET_IP4_DF)) == 0)
        {
            ip_hdr_size = (fnet_size_t)((((fnet_ip4_header_t *)ip_hdr)->version__header_length & 0x0F) << 2);
            protocol = ((fnet_ip4_header_t *)ip_hdr)->protocol;
        }
    }
    /* IPv6 */
    else if((eth_hdr->type == FNET_HTONS(FNET_ETH_TYPE_IP6)) && (datagram_size >= sizeof(fnet_ip6_header_t)))
    {
        ip_hdr_size = sizeof(fnet_ip6_header_t);
        protocol = ((fnet_ip6_header_t *)ip_hdr)->next_header;
    }

    /* If an IP frame with a known protocol is transmitted, the checksum is inserted automatically into the
     * frame.
     * The checksum field MUST be cleared.*/
    if(protocol)
    {
        prot_hdr = ip_hdr + ip_hdr_size;

        /* Clear checksum field.*/
        switch(protocol)
        {
            case FNET_PROT_ICMP4:
                if(datagram_size >= (ip_hdr_size + sizeof(fnet_icmp4_header_t))) /*Check length.*/
                    ((fnet_icmp4_header_t *)(prot_hdr))->checksum = 0;
                break;
            case FNET_PROT_UDP:
                if(datagram_size >= (ip_hdr_size + sizeof(fnet_udp_header_t))) /*Check length.*/
                    ((fnet_udp_header_t *)(prot_hdr))->checksum = 0;
                break;
            case FNET_PROT_TCP:
                if(datagram_size >= (ip_hdr_size + sizeof(fnet_tcp_header_t))) /*Check length.*/
                    ((fnet_tcp_header_t *)(prot_hdr))->checksum = 0;
                break;
        }
    }
}
#endif /* FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM */

#ifdef FNET_FEC_TEST_RACE_CONDITION
    fnet_index_t fnet_fec_output_reentry_count;
#endif

/************************************************************************
* DESCRIPTION: Ethernet low-level output function.
*************************************************************************/
void fnet_fec_output(fnet_netif_t *netif, fnet_netbuf_t *nb)
{
    fnet_fec_if_t       *fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv;
    fnet_uint8_t        *tx_buffer;

    if(nb && (nb->total_length >= FNET_ETH_HDR_SIZE))
    {
        /* fec_if->tx_buf_desc_cur->status will be updated by FEC-DMA so we need to invalidate cache */
        fnet_cpu_cache_invalidate();

        while((fec_if->tx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_TX_BD_R)) != 0u)
        {} /* If you are blocked here forever, there is communication issue between ENET and PHY.*/

        tx_buffer = (fnet_uint8_t *)fnet_ntohl((fnet_uint32_t)fec_if->tx_buf_desc_cur->buf_ptr);

        fnet_netbuf_to_buf(nb, 0u, FNET_NETBUF_COPYALL, tx_buffer);

#if FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM && FNET_FEC_HW_TX_PROTOCOL_CHECKSUM_FIX
        /* If an IP frame with a known protocol is transmitted,
         * the checksum is inserted automatically into the frame.
         * The checksum field MUST be cleared.
         * This is workaround, in case the checksum is not cleared.*/
        if((nb->flags & FNET_NETBUF_FLAG_HW_PROTOCOL_CHECKSUM) == 0)
        {
            fnet_fec_checksum_clear(tx_buffer, nb->total_length);
        }
#endif

        fec_if->tx_buf_desc_cur->length = fnet_htons((fnet_uint16_t)(nb->total_length));
        fec_if->tx_buf_desc_cur->status |= FNET_HTONS(FNET_FEC_TX_BD_R); /* Set Frame ready for transmit.*/

#ifdef FNET_FEC_TEST_RACE_CONDITION
        if (fnet_fec_output_reentry_count)
        {
            fnet_fec_output_reentry_count = 0;  /* Place breakpoint here.*/
            while(1) {};
        }
        fnet_fec_output_reentry_count = 1;
#endif
        /* Update pointer to next entry.*/
        if ((fec_if->tx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_TX_BD_W)) != 0u)
        {
            fec_if->tx_buf_desc_cur = fec_if->tx_buf_desc;
        }
        else
        {
            fec_if->tx_buf_desc_cur++;
        }

        while(fec_if->reg->TDAR) /* Workaround.*/
        {}

#ifdef FNET_FEC_TEST_RACE_CONDITION
        {
            fnet_index_t i;
            for (i = 0u; i < 10000u; i++) {}   /* tempo 20�s */
        }
#endif

        fec_if->reg->TDAR = FNET_FEC_TDAR_X_DES_ACTIVE; /* Indicate that there has been a transmit buffer produced.*/

#if !FNET_CFG_CPU_ETH_MIB
        fec_if->statistics.tx_packet++;
#endif
    }

#ifdef FNET_FEC_TEST_RACE_CONDITION
    fnet_fec_output_reentry_count = 0;
#endif

    fnet_netbuf_free_chain(nb);
}

/************************************************************************
* DESCRIPTION: Ethernet low-level output frame function.
*   !!!! Used for debug needs only!!!!!
*************************************************************************/
void fnet_fec_output_frame(fnet_netif_t *netif, fnet_uint8_t *frame, fnet_size_t frame_size)
{
    fnet_fec_if_t       *fec_if =  (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv;
    fnet_eth_header_t   *ethheader;

    if((frame != 0U) && (frame_size <= netif->netif_mtu))
    {
        /* fec_if->tx_buf_desc_cur->status will be updated by FEC-DMA so we need to invalidate cache */
        fnet_cpu_cache_invalidate();

        while((fec_if->tx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_TX_BD_R)) != 0u)
        {}

        ethheader = (fnet_eth_header_t *)fnet_ntohl((fnet_uint32_t)fec_if->tx_buf_desc_cur->buf_ptr);

        fnet_memcpy (ethheader, frame, frame_size);

        fnet_eth_trace("\nTX", ethheader); /* Print ETH header.*/

        fec_if->tx_buf_desc_cur->length = fnet_htons((fnet_uint16_t)(frame_size));
        fec_if->tx_buf_desc_cur->status |= FNET_HTONS(FNET_FEC_TX_BD_R); /* Set Frame ready for transmit.*/

        /* Update pointer to next entry.*/
        if ((fec_if->tx_buf_desc_cur->status & FNET_HTONS(FNET_FEC_RX_BD_W)) != 0u)
        {
            fec_if->tx_buf_desc_cur = fec_if->tx_buf_desc;
        }
        else
        {
            fec_if->tx_buf_desc_cur++;
        }

        while(fec_if->reg->TDAR) /* Workaround for ENET module.*/
        {}

        fec_if->reg->TDAR = FNET_FEC_TDAR_X_DES_ACTIVE; /* Indicate that there has been a transmit buffer produced.*/

#if !FNET_CFG_CPU_ETH_MIB
        fec_if->statistics.tx_packet++;
#endif
    }
}

/************************************************************************
* DESCRIPTION: Sets MAC address.
*************************************************************************/
static fnet_return_t fnet_fec_set_hw_addr(fnet_netif_t *netif, fnet_uint8_t *hw_addr)
{
    fnet_fec_if_t *fec_if;
    fnet_return_t result;

    /* Set the source address for the controller. */
    if(netif
       && (netif->netif_api->netif_type == FNET_NETIF_TYPE_ETHERNET)
       && ((fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv) != 0)
       && hw_addr
       && (fnet_memcmp(hw_addr, fnet_eth_null_addr, sizeof(fnet_mac_addr_t)))
       && (fnet_memcmp(hw_addr, fnet_eth_broadcast, sizeof(fnet_mac_addr_t)))
       && ((hw_addr[0] & 0x01U) == 0x00U)) /* Most significant nibble should be always even.*/
    {
        fec_if->reg->PALR = (fnet_uint32_t)(((fnet_uint32_t)hw_addr[0] << 24U) | ((fnet_uint32_t)hw_addr[1] << 16U) | ((fnet_uint32_t)hw_addr[2] << 8U) | ((fnet_uint32_t)hw_addr[3] << 0U));
        fec_if->reg->PAUR = (fnet_uint32_t)((fnet_uint32_t)hw_addr[4] << 24U) | ((fnet_uint32_t)hw_addr[5] << 16U);

        fnet_eth_change_addr_notify(netif);

        result = FNET_OK;
    }
    else
    {
        result = FNET_ERR;
    }

    return result;
}

/************************************************************************
* DESCRIPTION: This function reads HW address.
*************************************************************************/
static fnet_return_t fnet_fec_get_hw_addr(fnet_netif_t *netif, fnet_uint8_t *hw_addr)
{
    fnet_fec_if_t *fec_if ;
    fnet_return_t result;

    if(netif && (netif->netif_api->netif_type == FNET_NETIF_TYPE_ETHERNET)
       && ((fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv) != FNET_NULL)
       && (hw_addr) )
    {
        fnet_fec_get_mac_addr(fec_if, (fnet_mac_addr_t *) hw_addr);
        result = FNET_OK;
    }
    else
    {
        result = FNET_ERR;
    }

    return result;
}

/************************************************************************
* DESCRIPTION: This function reads MAC address.
*************************************************************************/
static void fnet_fec_get_mac_addr(fnet_fec_if_t *fec_if, fnet_mac_addr_t *mac_addr)
{
    fnet_uint32_t tmp;

    tmp = fec_if->reg->PALR;
    (*mac_addr)[0] = (fnet_uint8_t)(tmp >> 24);
    (*mac_addr)[1] = (fnet_uint8_t)(tmp >> 16);
    (*mac_addr)[2] = (fnet_uint8_t)(tmp >> 8);
    (*mac_addr)[3] = (fnet_uint8_t)tmp;

    tmp = fec_if->reg->PAUR;
    (*mac_addr)[4] = (fnet_uint8_t)(tmp >> 24);
    (*mac_addr)[5] = (fnet_uint8_t)(tmp >> 16);
}

/************************************************************************
* DESCRIPTION: Returns Ethernet statistics information
*************************************************************************/
static fnet_return_t fnet_fec_get_statistics(fnet_netif_t *netif, struct fnet_netif_statistics *statistics)
{
    fnet_fec_if_t *fec_if;
    fnet_return_t result;

    if(netif && (netif->netif_api->netif_type == FNET_NETIF_TYPE_ETHERNET))
    {
        fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv;
#if FNET_CFG_CPU_ETH_MIB
        statistics->tx_packet = fec_if->reg->RMON_T_PACKETS;
        statistics->rx_packet = fec_if->reg->RMON_R_PACKETS;
#else
        *statistics = fec_if->statistics;
#endif
        result = FNET_OK;
    }
    else
    {
        result = FNET_ERR;
    }

    return result;
}

/************************************************************************
* DESCRIPTION: Top Ethernet receive frame interrupt handler.
*              Clear event flag
*************************************************************************/
static void fnet_fec_isr_rx_handler_top (void *cookie)
{
    fnet_fec_if_t *fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(((fnet_netif_t *)cookie)->netif_prv))->eth_prv;

    /* Clear FEC RX Event from the Event Register (by writing 1).*/
    fec_if->reg->EIR = FNET_FEC_EIR_RXF;
}

/************************************************************************
* DESCRIPTION: This function implements the Ethernet receive
*              frame interrupt handler.
*************************************************************************/
static void fnet_fec_isr_rx_handler_bottom (void *cookie)
{
    fnet_netif_t *netif = (fnet_netif_t *)cookie;

    fnet_isr_lock();

    fnet_fec_input(netif);

    fnet_isr_unlock();
}

/************************************************************************
* MII Staff
*************************************************************************/

/************************************************************************
* DESCRIPTION: Looking for a valid PHY address.
*************************************************************************/
#if FNET_CFG_CPU_ETH_PHY_ADDR_DISCOVER
static void fnet_fec_phy_discover_addr (fnet_fec_if_t *fec_if, fnet_uint8_t phy_addr_start)
{
    fnet_uint8_t i;
    fnet_uint8_t phy_addr = fec_if->phy_addr; /* Save old value just in case the discover
                                               * is failed, in case ommunication with
                                               * the PHY via MDIO is not possible.*/

    for (i = (fnet_uint8_t)phy_addr_start; i < 32U; i++)
    {
        fnet_uint16_t id;

        fec_if->phy_addr = i;
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_IDR1, &id);

        if (!((id == 0U) || (id == 0xffffU) || (id == 0x7fffU)))
        {
            return; /* FHY address is discovered.*/
        }
    }
    fec_if->phy_addr = phy_addr;
}
#endif

/************************************************************************
* DESCRIPTION: Read a value from a PHY's MII register.
* reg_addr < address of the register in the PHY
* data < Pointer to storage for the Data to be read from the PHY register (passed by reference)
* Return FNET_ERR on failure, FNET_OK on success
*************************************************************************/
fnet_return_t fnet_fec_mii_read(fnet_fec_if_t *fec_if, fnet_uint32_t reg_addr, fnet_uint16_t *data)
{
    fnet_time_t       timeout;
    fnet_uint32_t     eimr;
    fnet_return_t     result;

    /* Clear the MII interrupt bit */
    fec_if->reg_phy->EIR = FNET_FEC_EIR_MII;

    /* Mask the MII interrupt */
    eimr = fec_if->reg_phy->EIMR;
    fec_if->reg_phy->EIMR &= (fnet_uint32_t)(~FNET_FEC_EIMR_MII);

    /* Kick-off the MII read */
    fec_if->reg_phy->MMFR = (fnet_vuint32_t)(0U
                                            | FNET_FEC_MMFR_ST_01
                                            | FNET_FEC_MMFR_OP_READ
                                            | FNET_FEC_MMFR_PA((fnet_uint32_t)fec_if->phy_addr)
                                            | FNET_FEC_MMFR_RA(reg_addr)
                                            | FNET_FEC_MMFR_TA_10);

    /* Poll for the MII interrupt */
    for (timeout = 0U; timeout < FNET_FEC_MII_TIMEOUT; timeout++)
    {
        if((fec_if->reg_phy->EIR & FNET_FEC_EIR_MII) != 0u)
        {
            break;
        }
    }

    /* Clear the MII interrupt bit */
    fec_if->reg_phy->EIR = FNET_FEC_EIR_MII;

    /* Restore the EIMR */
    fec_if->reg_phy->EIMR = eimr;

    if(timeout == FNET_FEC_MII_TIMEOUT)
    {
        result = FNET_ERR;
    }
    else
    {
        *data = (fnet_uint16_t)(fec_if->reg_phy->MMFR & 0xFFFFU);
        result = FNET_OK;
    }

    return result;
}

/************************************************************************
* DESCRIPTION: Write a value to a PHY's MII register.
* reg_addr < address of the register in the PHY
* data < Data to be writen to the PHY register (passed by reference)
* Return FNET_ERR on failure (timeout), FNET_OK on success
*************************************************************************/
fnet_return_t fnet_fec_mii_write(fnet_fec_if_t *fec_if, fnet_uint32_t reg_addr, fnet_uint16_t data)
{
    fnet_time_t     timeout;
    fnet_uint32_t   eimr;
    fnet_return_t   result;

    /* Clear the MII interrupt bit */
    fec_if->reg_phy->EIR = FNET_FEC_EIR_MII;

    /* Mask the MII interrupt */
    eimr = fec_if->reg_phy->EIMR;
    fec_if->reg_phy->EIMR &= (fnet_uint32_t)(~FNET_FEC_EIMR_MII);

    /* Kick-off the MII write */
    fec_if->reg_phy->MMFR = (fnet_vuint32_t)(0U
                                            | FNET_FEC_MMFR_ST_01
                                            | FNET_FEC_MMFR_OP_WRITE
                                            | FNET_FEC_MMFR_PA((fnet_uint32_t)fec_if->phy_addr)
                                            | FNET_FEC_MMFR_RA((fnet_uint32_t)reg_addr)
                                            | FNET_FEC_MMFR_TA_10
                                            | (fnet_uint32_t)(data & 0xffffU));

    /* Poll for the MII interrupt */
    for (timeout = 0U; timeout < FNET_FEC_MII_TIMEOUT; timeout++)
    {
        if((fec_if->reg_phy->EIR & FNET_FEC_EIR_MII) != 0u)
        {
            break;
        }
    }

    /* Clear the MII interrupt bit */
    fec_if->reg_phy->EIR = FNET_FEC_EIR_MII;

    /* Restore the EIMR */
    fec_if->reg_phy->EIMR = eimr;

    if(timeout == FNET_FEC_MII_TIMEOUT)
    {
        result = FNET_ERR;
    }
    else
    {
        result = FNET_OK;
    }

    return result;
}

/************************************************************************
* DESCRIPTION: Link status.
*************************************************************************/
static fnet_bool_t fnet_fec_is_connected(fnet_netif_t *netif)
{
    fnet_uint16_t   data;
    fnet_fec_if_t   *fec_if;
    fnet_bool_t     res = FNET_FALSE;

    fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv;

    /* Some PHY (e.g.DP8340) returns "unconnected" and than "connected" state
     *  just to show that was transition event from one state to another.
     *  As we need only curent state,  read 2 times and returtn
     *  the current/latest state.
     */
    fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_SR, &data);

    if (fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_SR, &data) == FNET_OK)
    {
        res = (((data & FNET_FEC_MII_REG_SR_LINK_STATUS) != 0u) ? FNET_TRUE : FNET_FALSE);
    }
    else /* Return previous value in case read PHY error. */
    {
        res = netif->is_connected;
    }

    return res;
}

/************************************************************************
* DESCRIPTION: Compute the CRC-32 polynomial on the multicast group
*************************************************************************/
#if FNET_CFG_MULTICAST
static fnet_uint32_t fnet_fec_crc_hash(fnet_mac_addr_t multicast_addr )
{
    fnet_uint32_t    crc = 0xFFFFFFFFu;
    fnet_index_t     i;
    fnet_index_t     j;

    for (i = 0u; i < 6u; i++)
    {
        fnet_uint8_t c = multicast_addr[i];
        for (j = 0u; j < 8u; j++)
        {
            if (((c ^ crc) & 1u) != 0u)
            {
                crc >>= 1;
                c >>= 1;
                crc ^= 0xEDB88320u;
            }
            else
            {
                crc >>= 1;
                c >>= 1;
            }
        }
    }
    return  crc;
}

/************************************************************************
* DESCRIPTION: Joins a multicast group on FEC interface.
*************************************************************************/
void fnet_fec_multicast_join(fnet_netif_t *netif, fnet_mac_addr_t multicast_addr )
{
    fnet_fec_if_t   *fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv;
    fnet_uint32_t   reg_value;
    fnet_uint32_t   crc;

    /* Set the appropriate bit in the hash table */
    crc = fnet_fec_crc_hash(multicast_addr );
    crc >>= 26;
    crc &= 0x3FU;

    reg_value = (fnet_uint32_t)(0x1U << (crc & 0x1FU));


    if (crc < 32U)
    {
        if((fec_if->reg->GALR & reg_value) != 0u)
        {
            fec_if->GALR_double |= reg_value;       /* Set double flag => never released.*/
        }
        else
        {
            fec_if->reg->GALR |= reg_value;
        }
    }
    else
    {
        if((fec_if->reg->GAUR & reg_value) != 0u)
        {
            fec_if->GAUR_double |= reg_value;       /* Set double flag => never released.*/
        }
        else
        {
            fec_if->reg->GAUR |= reg_value;
        }
    }
}

/************************************************************************
* DESCRIPTION: Leavess a multicast group on FEC interface.
*************************************************************************/
void fnet_fec_multicast_leave(fnet_netif_t *netif, fnet_mac_addr_t multicast_addr )
{
    fnet_fec_if_t   *fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv;
    fnet_uint32_t   reg_value;
    fnet_uint32_t   crc;

    /* Set the appropriate bit in the hash table */
    crc = fnet_fec_crc_hash(multicast_addr );
    crc >>= 26;
    crc &= 0x3FU;

    reg_value = (fnet_uint32_t)(0x1U << (crc & 0x1FU));

    if (crc < 32U)
    {
        fec_if->reg->GALR  = fec_if->reg->GALR & ( (~reg_value) | fec_if->GALR_double );
    }
    else
    {
        fec_if->reg->GAUR = fec_if->reg->GAUR & ((~reg_value) | fec_if->GAUR_double);
    }
}

#endif /*FNET_CFG_MULTICAST*/

/************************************************************************
* DESCRIPTION: This function polls fec driver.
* I has sence only if
* !!!! Used only for debug needs. !!!!!
*************************************************************************/
void fnet_fec_poll(fnet_netif_desc_t netif_desc)
{
    fnet_netif_t    *netif = (fnet_netif_t *)netif_desc;

    fnet_isr_lock();

    fnet_fec_input(netif);

    fnet_isr_unlock();
}

/************************************************************************
* DESCRIPTION: Prints all MII register.
* !!!! Used only for debug needs. !!!!!
*************************************************************************/
void fnet_fec_debug_mii_print_regs(fnet_netif_t *netif)
{
    fnet_fec_if_t *fec_if;
    fnet_uint16_t reg_value;

    if(netif->netif_api->netif_type == FNET_NETIF_TYPE_ETHERNET)
    {
        fec_if = (fnet_fec_if_t *)((fnet_eth_if_t *)(netif->netif_prv))->eth_prv;

        fnet_printf(" === MII registers ===\r\n");
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_CR, &reg_value);
        fnet_printf("\tCR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_SR, &reg_value);
        fnet_printf("\tSR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_IDR1, &reg_value);
        fnet_printf("\tIDR1 = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_IDR2, &reg_value);
        fnet_printf("\tIDR2 = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_ANAR, &reg_value);
        fnet_printf("\tANAR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_ANLPAR, &reg_value);
        fnet_printf("\tANLPAR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_ANER, &reg_value);
        fnet_printf("\tANER = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_ANNPTR, &reg_value);
        fnet_printf("\tANNPTR = 0x%04X\n", reg_value );
#if FNET_CFG_CPU_MPC5744P
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_PHYSTS, &reg_value);
        fnet_printf("\tPHYSTS = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_ICR, &reg_value);
        fnet_printf("\tICR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_ISR, &reg_value);
        fnet_printf("\tISR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_FCSCR, &reg_value);
        fnet_printf("\tFCSCR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_RECR, &reg_value);
        fnet_printf("\tRECR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_PCSR, &reg_value);
        fnet_printf("\tPCSR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_RBR, &reg_value);
        fnet_printf("\tRBR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_LEDCR, &reg_value);
        fnet_printf("\tLEDCR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_PHYCR, &reg_value);
        fnet_printf("\tPHYCR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_10BTSCR, &reg_value);
        fnet_printf("\t10BTSCR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_CDCTRL1, &reg_value);
        fnet_printf("\tCDCTRL1 = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_EDCR, &reg_value);
        fnet_printf("\tEDCR = 0x%04X\n", reg_value );
        /* TODO printing of the RGMII PHY registers */
#elif FNET_CFG_CPU_S32R274
#else
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_ICR, &reg_value);
        fnet_printf("\tICR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_PSR, &reg_value);
        fnet_printf("\tPSR = 0x%04X\n", reg_value );
        fnet_fec_mii_read(fec_if, FNET_FEC_MII_REG_PCR, &reg_value);
        fnet_printf("\tPCR = 0x%04X\n", reg_value );
#endif
    }
}

#endif /* (FNET_MCF || FNET_MK) && FNET_CFG_ETH */
