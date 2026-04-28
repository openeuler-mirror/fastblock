package volumeid

import "testing"

func TestEncodeDecodeRoundTrip(t *testing.T) {
	encoded, err := Encode(ID{ClusterID: "cluster-a", PoolID: 1, ImageID: 9})
	if err != nil {
		t.Fatalf("encode failed: %v", err)
	}
	decoded, err := Decode(encoded)
	if err != nil {
		t.Fatalf("decode failed: %v", err)
	}
	if decoded.ClusterID != "cluster-a" || decoded.PoolID != 1 || decoded.ImageID != 9 {
		t.Fatalf("unexpected decoded id: %+v", decoded)
	}
}

func TestDecodeRejectsInvalidFormat(t *testing.T) {
	if _, err := Decode("bad-id"); err == nil {
		t.Fatal("expected decode to fail")
	}
}

func TestEncodeDecodeNameRef(t *testing.T) {
	encoded, err := EncodeNameRef(NameRef{Pool: "fb", Name: "img-a"})
	if err != nil {
		t.Fatalf("encode name ref failed: %v", err)
	}
	decoded, err := DecodeNameRef(encoded)
	if err != nil {
		t.Fatalf("decode name ref failed: %v", err)
	}
	if decoded.Pool != "fb" || decoded.Name != "img-a" {
		t.Fatalf("unexpected decoded name ref: %+v", decoded)
	}
}
