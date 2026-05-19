package api

type CreateExportRequest struct {
	VolumeID      string
	PoolName      string
	ImageName     string
	CapacityBytes int64
	BlockSize     int64
	Transport     string
}

type Export struct {
	ID      string
	NQN     string
	NSID    int
	Traddr  string
	Trsvcid string
}
