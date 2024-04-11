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
package config

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"time"
)

const (
	defaultEtcdStartTimeout = "5s"
	defaultEtcdMaxTTL       = "5s"

	defaultEtcdMasterRetryInterval = "5s"

	defaultRecheckPGInterval = "5s"

	defaultLogPath               = "/var/log/fastblock/monitor.log"
	defaultLogLevel              = "info"
	defaultMonitorHostName       = "fbmonitor"
	defaultMonitorAddress        = "127.0.0.1"
	defaultMonitorPort           = 3333
	defaultPrometheusMonitorPort = 3332
	defaultEtcdServerPort        = 2379
	defaultEtcdPort              = 2380
)

// TODO: where should we keep all these keys?
const (
	// Input
	ConfigPoolsKeyPrefix    = "/config/pools/"
	ConfigImagesKeyPrefix   = "/config/images/"
	ConfigOSDStatsKeySuffix = "/osd/stats/"
	ConfigOSDMapKey         = "/osd/osdmap"
	ConfigTopologyKeyPrefix = "/config/topology/"
	ConfigHostsPrefix       = ConfigTopologyKeyPrefix + "hosts/"
	ConfigRacksPrefix       = ConfigTopologyKeyPrefix + "racks/"
	ConfigRootsPrefix       = ConfigTopologyKeyPrefix + "roots/"
)

// Config types.
type Config struct {
	Monitors           []string `json:"monitors"`
	MonHost            []string `json:"mon_host"`
	EtcdServer         []string `json:"etcd_server"`
	EtcdName           string   `json:"etcd_name"`
	EtcdInitialCluster string   `json:"etcd_initial_cluster"`
	EtcdACUrls         []string `json:"etcd_advertise_client_urls"`
	EtcdAPUrls         []string `json:"etcd_advertise_peer_urls"`
	EtcdLPUrls         []string `json:"etcd_listen_peer_urls"`
	EtcdLCUrls         []string `json:"etcd_listen_client_urls"`
	EtcdPrefix         string   `json:"etcd_prefix"`
	EtcdStartTimeout   string   `json:"etcd_start_timeout"` // In seconds.
	EtcdMaxTTL         string   `json:"etcd_max_ttl"`

	ElectionMasterKey       string `json:"election_master_key"`
	EtcdMasterRetryInterval string `json:"etcd_master_retry_interval"`
	LogPath                 string `json:"log_path"`
	LogLevel                string `json:"log_level"` // "info", "warn", "error"
	HostName                string `json:"hostname"`
	Address                 string `json:"address"`
	DataDir                 string `json:"data_dir"`
	Port                    int    `json:"port"`
	PrometheusPort          int    `json:"prometheus_port"`
}

// CONFIG parsed.
var CONFIG Config

// SetupConfig parse and check config file.
func SetupConfig(configFilePath string, monitorId string) {
	marshalJsonConfig(configFilePath, monitorId)
}

func marshalJsonConfig(configFilePath string, monitorId string) error {
	data, err := ioutil.ReadFile(configFilePath)
	if err != nil {
		if err != nil {
			panic("Cannot open " + configFilePath)
		}
	}
	var c Config
	err = json.Unmarshal([]byte(data), &c)
	if err != nil {
		panic("load config file error: " + err.Error())
	}

	validate(&c)

	// setup CONFIG with defaults if necessary.
	c.EtcdStartTimeout = Ternary(c.EtcdStartTimeout == "", defaultEtcdStartTimeout, c.EtcdStartTimeout).(string)
	c.EtcdMaxTTL = Ternary(c.EtcdMaxTTL == "", defaultEtcdMaxTTL, c.EtcdMaxTTL).(string)

	c.EtcdMasterRetryInterval = Ternary(c.EtcdMasterRetryInterval == "", defaultEtcdMasterRetryInterval, c.EtcdMasterRetryInterval).(string)
	c.LogPath = Ternary(c.LogPath == "", defaultLogPath, c.LogPath).(string)
	c.LogLevel = Ternary(c.LogLevel == "", defaultLogLevel, c.LogLevel).(string)

	c.EtcdServer = []string{}
	c.EtcdACUrls = []string{}
	c.EtcdAPUrls = []string{}
	c.EtcdLPUrls = []string{}
	c.EtcdLCUrls = []string{}

	for i := range c.Monitors {
		if c.Monitors[i] == monitorId {
			c.EtcdName = c.Monitors[i]
			c.HostName = c.Monitors[i]
			c.EtcdInitialCluster = fmt.Sprintf("%s=http://%s:%d", c.Monitors[i], c.MonHost[i], defaultEtcdPort)
			curl := fmt.Sprintf("http://%s:%d", c.MonHost[i], defaultEtcdServerPort)
			c.EtcdACUrls = append(c.EtcdACUrls, curl)
			url := fmt.Sprintf("http://%s:%d", c.MonHost[i], defaultEtcdPort)
			c.EtcdAPUrls = append(c.EtcdAPUrls, url)
			c.EtcdLPUrls = append(c.EtcdLPUrls, url)

			local_url := fmt.Sprintf("http://127.0.0.1:%d", defaultEtcdServerPort)
			c.EtcdLCUrls = append(c.EtcdLCUrls, local_url)
			c.EtcdLCUrls = append(c.EtcdLCUrls, curl)
			c.Address = c.MonHost[i]

		}
		server := fmt.Sprintf("%s:%d", c.MonHost[i], defaultEtcdServerPort)
		c.EtcdServer = append(c.EtcdServer, server)
	}

	if c.Port == 0 {
		c.Port = defaultMonitorPort
	}

	if c.PrometheusPort == 0 {
		c.PrometheusPort = defaultPrometheusMonitorPort
	}

	if len(c.DataDir) == 0 {
		c.DataDir = "/tmp/mon_" + c.HostName
	}

	CONFIG = c
	return nil
}

func validate(config *Config) {
	if len(config.Monitors) == 0 {
		panic("Monitors invalid")
	}

	if len(config.MonHost) == 0 {
		panic("MonHost invalid")
	}

	if len(config.Monitors) != len(config.MonHost) {
		panic("Monitors and MonHost invalid")
	}

	if len(config.ElectionMasterKey) == 0 {
		panic("EtcdMasterID invalid")
	}
}

// Ternary is mimic `?:` operator
// Need type assertion to convert output to expected type
func Ternary(IF bool, THEN interface{}, ELSE interface{}) interface{} {
	if IF {
		return THEN
	} else {
		return ELSE
	}
}

// Duration wrapper.
type Duration struct {
	time.Duration
}

// UnmarshalText for toml to time.Duration.
func (d *Duration) UnmarshalText(text []byte) error {
	var err error
	d.Duration, err = time.ParseDuration(string(text))
	return err
}
