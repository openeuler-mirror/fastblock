package exporterclient

import "testing"

func TestExportIDForVolume(t *testing.T) {
	exportID, err := ExportIDForVolume("fbvolname:fb:img-a")
	if err != nil {
		t.Fatalf("derive export id failed: %v", err)
	}
	if exportID != "fbvolname-fb-img-a" {
		t.Fatalf("unexpected export id %q", exportID)
	}
}

func TestExportIDForVolumeRejectsEmptyValue(t *testing.T) {
	if _, err := ExportIDForVolume(""); err == nil {
		t.Fatal("expected empty volume id to fail")
	}
}
