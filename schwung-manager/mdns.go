package main

import (
	"encoding/binary"
	"log/slog"
	"net"
	"time"
)

// startMDNS launches a goroutine that listens on the mDNS multicast group
// (224.0.0.251:5353) and responds to A record queries for the given hostname
// with the device's current IP address. Retries setup until the network is ready.
func startMDNS(hostname string, logger *slog.Logger) {
	qname := encodeDNSName(hostname + ".")

	go func() {
		// Retry until network is available and multicast socket is bound.
		var conn *net.UDPConn
		for {
			ip := getOutboundIP()
			if ip == nil {
				logger.Info("mdns: waiting for network...")
				time.Sleep(5 * time.Second)
				continue
			}

			mdnsAddr := &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}

			// Find the network interface for our outbound IP.
			var iface *net.Interface
			ifaces, _ := net.Interfaces()
			for i := range ifaces {
				addrs, _ := ifaces[i].Addrs()
				for _, a := range addrs {
					if ipnet, ok := a.(*net.IPNet); ok && ipnet.IP.Equal(ip) {
						iface = &ifaces[i]
						break
					}
				}
				if iface != nil {
					break
				}
			}

			var err error
			conn, err = net.ListenMulticastUDP("udp4", iface, mdnsAddr)
			if err != nil {
				logger.Error("mdns: listen failed, retrying", "err", err, "iface", iface)
				time.Sleep(5 * time.Second)
				continue
			}

			ifName := "any"
			if iface != nil {
				ifName = iface.Name
			}
			logger.Info("mdns: advertising", "hostname", hostname, "ip", ip.String(), "iface", ifName)
			break
		}

		buf := make([]byte, 1500)
		for {
			n, _, err := conn.ReadFromUDP(buf)
			if err != nil {
				continue
			}
			if n < 12 {
				continue
			}

			// Parse DNS header: questions count at offset 4-5
			qdcount := binary.BigEndian.Uint16(buf[4:6])
			if qdcount == 0 {
				continue
			}

			// Walk questions looking for our hostname with type A (1), class IN (1)
			offset := 12
			for i := 0; i < int(qdcount) && offset < n; i++ {
				nameStart := offset
				// Skip the name
				for offset < n {
					length := int(buf[offset])
					if length == 0 {
						offset++
						break
					}
					if length >= 0xC0 { // pointer
						offset += 2
						break
					}
					offset += 1 + length
				}
				if offset+4 > n {
					break
				}
				qtype := binary.BigEndian.Uint16(buf[offset : offset+2])
				qclass := binary.BigEndian.Uint16(buf[offset+2 : offset+4])
				offset += 4

				// Check: type A (1), class IN (1 or 0x8001 for unicast-response)
				if qtype == 1 && (qclass == 1 || qclass == 0x8001) {
					nameLen := offset - 4 - nameStart
					if nameLen == len(qname) && string(buf[nameStart:nameStart+nameLen]) == string(qname) {
						// Get current IP (may change over time)
						ip := getOutboundIP()
						if ip != nil {
							mdnsAddr := &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}
							resp := buildMDNSResponse(qname, ip)
							conn.WriteToUDP(resp, mdnsAddr)
						}
					}
				}
			}
		}
	}()
}

// buildMDNSResponse builds a minimal DNS response with a single A record.
func buildMDNSResponse(qname []byte, ip net.IP) []byte {
	resp := make([]byte, 0, 64)

	// Header: ID=0, flags=0x8400 (response, authoritative), 0 questions, 1 answer
	resp = append(resp, 0, 0)       // ID
	resp = append(resp, 0x84, 0x00) // Flags: response, authoritative
	resp = append(resp, 0, 0)       // QDCOUNT
	resp = append(resp, 0, 1)       // ANCOUNT
	resp = append(resp, 0, 0)       // NSCOUNT
	resp = append(resp, 0, 0)       // ARCOUNT

	// Answer: name, type A, class IN (cache-flush), TTL 120, RDLENGTH 4, IP
	resp = append(resp, qname...)
	resp = append(resp, 0, 1)          // TYPE A
	resp = append(resp, 0x80, 1)       // CLASS IN with cache-flush bit
	resp = append(resp, 0, 0, 0, 120)  // TTL 120 seconds
	resp = append(resp, 0, 4)          // RDLENGTH
	resp = append(resp, ip.To4()...)

	return resp
}

// encodeDNSName encodes a dotted name into DNS wire format.
func encodeDNSName(name string) []byte {
	var result []byte
	for len(name) > 0 {
		dot := 0
		for dot < len(name) && name[dot] != '.' {
			dot++
		}
		result = append(result, byte(dot))
		result = append(result, name[:dot]...)
		if dot < len(name) {
			name = name[dot+1:]
		} else {
			name = ""
		}
	}
	result = append(result, 0) // terminator
	return result
}

// getOutboundIP returns the device's primary outbound IP address.
func getOutboundIP() net.IP {
	conn, err := net.Dial("udp4", "1.0.0.0:80")
	if err != nil {
		return nil
	}
	defer conn.Close()
	return conn.LocalAddr().(*net.UDPAddr).IP
}
