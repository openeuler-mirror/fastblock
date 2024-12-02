/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
package utils

import (
	"fmt"
	"strconv"
	"strings"
)

func SpeedToString(speedValue uint64) string {
	units := [...]string{" B/s", " kB/s", " MB/s", " GB/s", " TB/s"}
	speed := float64(speedValue)

	for i := 0; i < len(units); i++ {
		if speed < 1024 {
			// Format with 2 decimal places, but trim trailing zeros
			s := strconv.FormatFloat(speed, 'f', 2, 64)
			s = strings.TrimRight(strings.TrimRight(s, "0"), ".")
			return s + units[i]
		}
		speed /= 1024
	}

	// If we get here, format the final TB/s value
	s := strconv.FormatFloat(speed, 'f', 2, 64)
	s = strings.TrimRight(strings.TrimRight(s, "0"), ".")
	return s + units[len(units)-1]
}

func PrintStatus(cliReadBytes uint64, cliWriteBytes uint64, cliReadIops uint64, cliWriteIops uint64, recoveryBytes uint64, recoveryObjPs uint64) string {
	var result strings.Builder
	client := make(map[string]string)
	recovery := make(map[string]string)
	rd := "rd"
	wr := "wr"
	ord := "op/s rd"
	owr := "op/s wr"
	speed := "speed"
	ops := "ops"
	order := [...]string{rd, wr, ord, owr}
	recoveryOrder := [...]string{speed, ops}

	if cliReadBytes != 0 {
		client[rd] = SpeedToString(cliReadBytes)
	}
	if cliWriteBytes != 0 {
		client[wr] = SpeedToString(cliWriteBytes)
	}
	if cliReadIops != 0 {
		client[ord] = strconv.FormatUint(cliReadIops, 10)
	}
	if cliWriteIops != 0 {
		client[owr] = strconv.FormatUint(cliWriteIops, 10)
	}

	if recoveryBytes != 0 {
		recovery[speed] = SpeedToString(recoveryBytes)
	}
	if recoveryObjPs != 0 {
		recovery[ops] = strconv.FormatUint(recoveryObjPs, 10) + " objects/s"
	}

	if len(client) != 0 || len(recovery) != 0 {
		result.WriteString("  io: \r\n")
		if len(client) != 0 {
			result.WriteString("    client   : ")
			i := 0
			for _, units := range order {
				if data, ok := client[units]; ok {
					if i != 0 {
						result.WriteString(",  ")
					}
					result.WriteString(fmt.Sprintf("%v %v", data, units))
					i += 1
				}
			}
			result.WriteString("\n")
		}

		if len(recovery) != 0 {
			result.WriteString("    recovery : ")
			i := 0
			for _, val := range recoveryOrder {
				if data, ok := recovery[val]; ok {
					if i != 0 {
						result.WriteString(",  ")
					}
					result.WriteString(data)
					i += 1
				}
			}
			result.WriteString("\n")
		}
	}

	return result.String()
}
