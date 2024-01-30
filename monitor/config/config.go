/* Copyright (c) 2024 ChinaUnicom
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
	"io/ioutil"
	"time"

	"github.com/BurntSushi/toml"
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
	EtcdServer         []string `toml:"etcd_server"`
	EtcdName           string   `toml:"etcd_name"`
	EtcdInitialCluster string   `toml:"etcd_initial_cluster"`
	EtcdACUrls         []string `toml:"etcd_advertise_client_urls"`
	EtcdAPUrls         []string `toml:"etcd_advertise_peer_urls"`
	EtcdLPUrls         []string `toml:"etcd_listen_peer_urls"`
	EtcdLCUrls         []string `toml:"etcd_listen_client_urls"`
	EtcdPrefix         string   `toml:"etcd_prefix"`
	EtcdStartTimeout   string   `toml:"etcd_start_timeout"` // In seconds.
	EtcdMaxTTL         string   `toml:"etcd_max_ttl"`

	ElectionMasterKey       string `toml:"election_master_key"`
	EtcdMasterRetryInterval string `toml:"etcd_master_retry_interval"`
	LogPath                 string `toml:"log_path"`
	LogLevel                string `toml:"log_level"` // "info", "warn", "error"
	HostName                string `toml:"hostname"`
	Address                 string `toml:"address"`
	DataDir                 string `toml:"data_dir"`
	Port                    int    `toml:"port"`
	PrometheusPort          int    `toml:"prometheus_port"`
}

// CONFIG parsed.
var CONFIG Config

// SetupConfig parse and check config file.
func SetupConfig(configFilePath string) {
	marshalTOMLConfig(configFilePath)
}

func marshalTOMLConfig(configFilePath string) error {
	data, err := ioutil.ReadFile(configFilePath)
	if err != nil {
		if err != nil {
			panic("Cannot open " + configFilePath)
		}
	}
	var c Config
	_, err = toml.Decode(string(data), &c)
	if err != nil {
		panic("load monitor.toml error: " + err.Error())
	}

	validate(&c)

	// setup CONFIG with defaults if necessary.
	c.EtcdStartTimeout = Ternary(c.EtcdStartTimeout == "", defaultEtcdStartTimeout, c.EtcdStartTimeout).(string)
	c.EtcdMaxTTL = Ternary(c.EtcdMaxTTL == "", defaultEtcdMaxTTL, c.EtcdMaxTTL).(string)

	c.EtcdMasterRetryInterval = Ternary(c.EtcdMasterRetryInterval == "", defaultEtcdMasterRetryInterval, c.EtcdMasterRetryInterval).(string)
	c.LogPath = Ternary(c.LogPath == "", defaultLogPath, c.LogPath).(string)
	c.LogLevel = Ternary(c.LogLevel == "", defaultLogLevel, c.LogLevel).(string)
	c.HostName = Ternary(c.HostName == "", defaultMonitorHostName, c.HostName).(string)
	c.Address = Ternary(c.Address == "", defaultMonitorAddress, c.Address).(string)

	if c.Port == 0 {
		c.Port = defaultMonitorPort
	}
	if c.PrometheusPort == 0 {
		c.PrometheusPort = defaultPrometheusMonitorPort
	}

	CONFIG = c

	return nil
}

func validate(config *Config) {
	if len(config.EtcdServer) == 0 {
		panic("EtcdServer invalid")
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
