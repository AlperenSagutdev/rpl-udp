/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "cfs/cfs.h"
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678
#define BLOCK_SIZE 64

struct __attribute__((packed)) firmware_packet {
    uint16_t block_num;          // Sıralama (Sequence) numarası
    uint8_t data_len;            // Blok içindeki gerçek veri boyutu (Son blok < 64 olabilir)
    uint16_t checksum;           // Parça doğrulama için checksum
    uint8_t data[BLOCK_SIZE];    // 64 baytlık makine kodu (binary veri)
};

struct __attribute__((packed)) ack_packet {
    uint16_t block_num;          // Alınan/Onaylanan blok numarası
    uint8_t status;              // 1: ACK (Başarılı), 0: NACK (Hatalı parça)
};

uint16_t calculate_checksum(const uint8_t *data, uint8_t len) {
    uint16_t checksum = 0;
    for(uint8_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

static int out_fd = -1;
static uint16_t next_expected_block = 0;
static struct simple_udp_connection udp_conn;

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr, uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
         const uint8_t *data, uint16_t datalen)
{
  struct ack_packet response;

  if(datalen == sizeof(struct firmware_packet)) {
    struct firmware_packet *pkt = (struct firmware_packet *)data;
    
    uint16_t calc_chk = calculate_checksum(pkt->data, pkt->data_len);
    if(calc_chk != pkt->checksum) {
      LOG_ERR("Blok %u bozuk! (Checksum hatasi)\n", pkt->block_num);
      response.block_num = pkt->block_num;
      response.status = 0; // NACK
      simple_udp_sendto(&udp_conn, &response, sizeof(struct ack_packet), sender_addr);
      return;
    }

    if(pkt->block_num == next_expected_block) {
      // Doğru blok geldiyse diske yaz
      if(out_fd >= 0) {
        cfs_write(out_fd, pkt->data, pkt->data_len);
        LOG_INFO("Blok %u diske yazildi (%d bayt).\n", pkt->block_num, pkt->data_len);
      }
      next_expected_block++;
    } else if (pkt->block_num < next_expected_block) {
      LOG_INFO("Eski/Mükerrer blok %u alindi, tekrar ACK yollaniyor.\n", pkt->block_num);
    } else {
      LOG_WARN("Beklenmeyen blok %u geldi. Beklenen: %u\n", pkt->block_num, next_expected_block);
      return; 
    }

    response.block_num = pkt->block_num;
    response.status = 1; // ACK
    simple_udp_sendto(&udp_conn, &response, sizeof(struct ack_packet), sender_addr);
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();

  cfs_remove("received.elf");
  out_fd = cfs_open("received.elf", CFS_WRITE );
  if(out_fd < 0) {
    LOG_ERR("HATA: received.elf olusturulamadi!\n");
  }

  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                        UDP_CLIENT_PORT, udp_rx_callback);

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
