package rawproto

import (
	"bytes"
	"encoding/binary"
	"sort"

	"monitor/msg"
)

type clusterMapResponseHeader struct {
	OSDMapEpoch uint64
	PGMapEpoch  uint64
	OSDCount    uint32
	PGCount     uint32
}

type osdEntryHeader struct {
	OSDID      int32
	Flags      uint32
	AddressLen uint16
	ShardCount uint16
}

type osdShardEntry struct {
	ShardID uint32
	Port    uint16
	CoreID  uint16
}

type pgEntryHeader struct {
	PoolID       int32
	PGID         int32
	Version      int64
	State        uint32
	PrimaryShard uint32
	ReplicaCount uint32
}

func EncodeGetClusterMapResponse(
	seq uint64,
	status uint32,
	osdMapEpoch uint64,
	pgMapEpoch uint64,
	osds []*msg.OsdDynamicInfo,
	pgResp *msg.GetPgMapResponse,
) (Header, []byte, error) {
	body := bytes.NewBuffer(nil)
	pgCount := uint32(0)
	if pgResp != nil {
		for _, infos := range pgResp.GetPgs() {
			pgCount += uint32(len(infos.GetPi()))
		}
	}

	rspHdr := clusterMapResponseHeader{
		OSDMapEpoch: osdMapEpoch,
		PGMapEpoch:  pgMapEpoch,
		OSDCount:    uint32(len(osds)),
		PGCount:     pgCount,
	}
	if err := binary.Write(body, binary.LittleEndian, &rspHdr); err != nil {
		return Header{}, nil, err
	}

	sort.Slice(osds, func(i, j int) bool {
		return osds[i].GetOsdid() < osds[j].GetOsdid()
	})
	for _, osdInfo := range osds {
		if err := encodeOSDEntry(body, osdInfo); err != nil {
			return Header{}, nil, err
		}
	}
	if pgResp != nil {
		if err := encodePGEntries(body, pgResp); err != nil {
			return Header{}, nil, err
		}
	}

	return Header{
		Magic:        Magic,
		VersionMajor: VersionMajor,
		VersionMinor: VersionMinor,
		Service:      ServiceMonitor,
		Opcode:       OpGetClusterMap,
		Flags:        FlagResponse,
		Seq:          seq,
		Status:       status,
	}, body.Bytes(), nil
}

func encodeOSDEntry(body *bytes.Buffer, osdInfo *msg.OsdDynamicInfo) error {
	flags := uint32(0)
	if osdInfo.GetIsin() {
		flags |= 1 << 0
	}
	if osdInfo.GetIsup() {
		flags |= 1 << 1
	}
	shards := osdInfo.GetShardedPorts()
	shardIDs := make([]uint32, 0, len(shards))
	for shardID := range shards {
		shardIDs = append(shardIDs, shardID)
	}
	sort.Slice(shardIDs, func(i, j int) bool { return shardIDs[i] < shardIDs[j] })

	hdr := osdEntryHeader{
		OSDID:      osdInfo.GetOsdid(),
		Flags:      flags,
		AddressLen: uint16(len(osdInfo.GetAddress())),
		ShardCount: uint16(len(shardIDs)),
	}
	if err := binary.Write(body, binary.LittleEndian, &hdr); err != nil {
		return err
	}
	if _, err := body.WriteString(osdInfo.GetAddress()); err != nil {
		return err
	}
	for _, shardID := range shardIDs {
		core := shards[shardID]
		port := core.GetRawPort()
		if port == 0 {
			port = core.GetPort()
		}
		entry := osdShardEntry{
			ShardID: shardID,
			Port:    uint16(port),
			CoreID:  uint16(core.GetCoreid()),
		}
		if err := binary.Write(body, binary.LittleEndian, &entry); err != nil {
			return err
		}
	}
	return nil
}

func encodePGEntries(body *bytes.Buffer, pgResp *msg.GetPgMapResponse) error {
	poolIDs := make([]int, 0, len(pgResp.GetPgs()))
	for poolID := range pgResp.GetPgs() {
		poolIDs = append(poolIDs, int(poolID))
	}
	sort.Ints(poolIDs)
	for _, poolID := range poolIDs {
		infos := pgResp.GetPgs()[int32(poolID)]
		pgs := append([]*msg.PGInfo(nil), infos.GetPi()...)
		sort.Slice(pgs, func(i, j int) bool {
			return pgs[i].GetPgid() < pgs[j].GetPgid()
		})
		for _, pg := range pgs {
			hdr := pgEntryHeader{
				PoolID:       int32(poolID),
				PGID:         pg.GetPgid(),
				Version:      pg.GetVersion(),
				State:        uint32(pg.GetState()),
				PrimaryShard: pg.GetCoreindex(),
				ReplicaCount: uint32(len(pg.GetOsdid())),
			}
			if err := binary.Write(body, binary.LittleEndian, &hdr); err != nil {
				return err
			}
			for _, osdID := range pg.GetOsdid() {
				if err := binary.Write(body, binary.LittleEndian, osdID); err != nil {
					return err
				}
			}
		}
	}
	return nil
}
