package rawproto

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
)

const (
	Magic        uint32 = 0x46425257
	VersionMajor uint8  = 1
	VersionMinor uint8  = 0

	ServiceMonitor uint8 = 1

	OpGetImageInfo  uint8 = 1
	OpGetClusterMap uint8 = 2

	FlagResponse uint32 = 1 << 0

	StatusOK             uint32 = 0
	StatusInvalidRequest uint32 = 1
	StatusNotFound       uint32 = 2
	StatusStaleEpoch     uint32 = 3
	StatusRetryLater     uint32 = 4
	StatusNotLeader      uint32 = 5
	StatusPGInitializing uint32 = 6
	StatusOSDDown        uint32 = 7
	StatusInternalError  uint32 = 8
)

type Header struct {
	Magic        uint32
	VersionMajor uint8
	VersionMinor uint8
	Service      uint8
	Opcode       uint8
	Flags        uint32
	Seq          uint64
	Status       uint32
	BodyLen      uint32
}

type GetImageInfoRequest struct {
	ImageEpoch uint64
	PoolName   string
	ImageName  string
}

type GetClusterMapRequest struct {
	OSDMapEpoch uint64
	PoolID      uint32
	Reserved    uint32
	PGMapEpoch  uint64
}

func ReadHeader(r io.Reader) (*Header, error) {
	hdr := &Header{}
	if err := binary.Read(r, binary.LittleEndian, hdr); err != nil {
		return nil, err
	}
	return hdr, nil
}

func ReadBody(r io.Reader, bodyLen uint32) ([]byte, error) {
	if bodyLen == 0 {
		return nil, nil
	}
	buf := make([]byte, bodyLen)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

func WriteMessage(w io.Writer, hdr Header, body []byte) error {
	if body == nil {
		body = []byte{}
	}
	hdr.BodyLen = uint32(len(body))
	if err := binary.Write(w, binary.LittleEndian, &hdr); err != nil {
		return err
	}
	if len(body) == 0 {
		return nil
	}
	_, err := w.Write(body)
	return err
}

func ValidateRequestHeader(hdr *Header) error {
	if hdr == nil {
		return errors.New("nil header")
	}
	if hdr.Magic != Magic {
		return errors.New("invalid magic")
	}
	if hdr.VersionMajor != VersionMajor {
		return errors.New("unsupported version")
	}
	if hdr.Service != ServiceMonitor {
		return errors.New("invalid service")
	}
	if hdr.Flags&FlagResponse != 0 {
		return errors.New("unexpected response flag")
	}
	return nil
}

func MakeResponseHeader(req *Header, status uint32) Header {
	return Header{
		Magic:        Magic,
		VersionMajor: VersionMajor,
		VersionMinor: VersionMinor,
		Service:      ServiceMonitor,
		Opcode:       req.Opcode,
		Flags:        FlagResponse,
		Seq:          req.Seq,
		Status:       status,
	}
}

func DecodeGetImageInfoRequest(body []byte) (*GetImageInfoRequest, error) {
	reader := bytes.NewReader(body)
	var imageEpoch uint64
	var poolNameLen uint16
	var imageNameLen uint16

	if err := binary.Read(reader, binary.LittleEndian, &imageEpoch); err != nil {
		return nil, err
	}
	if err := binary.Read(reader, binary.LittleEndian, &poolNameLen); err != nil {
		return nil, err
	}
	if err := binary.Read(reader, binary.LittleEndian, &imageNameLen); err != nil {
		return nil, err
	}

	remaining := int(poolNameLen) + int(imageNameLen)
	if reader.Len() != remaining {
		return nil, errors.New("invalid image info request body length")
	}

	poolName := make([]byte, poolNameLen)
	imageName := make([]byte, imageNameLen)
	if _, err := io.ReadFull(reader, poolName); err != nil {
		return nil, err
	}
	if _, err := io.ReadFull(reader, imageName); err != nil {
		return nil, err
	}

	return &GetImageInfoRequest{
		ImageEpoch: imageEpoch,
		PoolName:   string(poolName),
		ImageName:  string(imageName),
	}, nil
}

func EncodeGetImageInfoResponse(
	seq uint64,
	status uint32,
	imageEpoch uint64,
	poolID uint32,
	blockSize uint32,
	objectSize uint32,
	flags uint32,
	sizeBytes uint64,
) (Header, []byte, error) {
	body := bytes.NewBuffer(make([]byte, 0, 32))
	for _, v := range []interface{}{imageEpoch, poolID, blockSize, objectSize, flags, sizeBytes} {
		if err := binary.Write(body, binary.LittleEndian, v); err != nil {
			return Header{}, nil, err
		}
	}
	return Header{
		Magic:        Magic,
		VersionMajor: VersionMajor,
		VersionMinor: VersionMinor,
		Service:      ServiceMonitor,
		Opcode:       OpGetImageInfo,
		Flags:        FlagResponse,
		Seq:          seq,
		Status:       status,
	}, body.Bytes(), nil
}

func DecodeGetClusterMapRequest(body []byte) (*GetClusterMapRequest, error) {
	reader := bytes.NewReader(body)
	var req GetClusterMapRequest
	if err := binary.Read(reader, binary.LittleEndian, &req); err != nil {
		return nil, err
	}
	if reader.Len() != 0 {
		return nil, errors.New("invalid cluster map request body length")
	}
	return &req, nil
}
