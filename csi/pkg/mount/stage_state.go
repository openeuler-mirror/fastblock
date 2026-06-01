package mount

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
)

type StageState struct {
	VolumeID   string `json:"volume_id"`
	DevicePath string `json:"device_path"`
	Transport  string `json:"transport"`
	NQN        string `json:"nqn"`
	Traddr     string `json:"traddr"`
	Trsvcid    string `json:"trsvcid"`
	NSID       int    `json:"nsid"`
}

func WriteStageState(stagePath string, state StageState) error {
	if stagePath == "" {
		return errors.New("stage path is required")
	}
	if err := ensureStagePathDirectory(stagePath); err != nil {
		return err
	}
	data, err := json.Marshal(state)
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(stagePath, "stage-state.json"), data, 0o644)
}

func ReadStageState(stagePath string) (StageState, error) {
	data, err := os.ReadFile(filepath.Join(stagePath, "stage-state.json"))
	if err != nil {
		return StageState{}, err
	}
	var state StageState
	if err := json.Unmarshal(data, &state); err != nil {
		return StageState{}, err
	}
	return state, nil
}

func RemoveStageState(stagePath string) error {
	if stagePath == "" {
		return errors.New("stage path is required")
	}
	err := os.Remove(filepath.Join(stagePath, "stage-state.json"))
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func ensureStagePathDirectory(stagePath string) error {
	info, err := os.Lstat(stagePath)
	if err == nil {
		if info.IsDir() {
			return nil
		}
		if err := os.Remove(stagePath); err != nil {
			return err
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	return os.MkdirAll(stagePath, 0o755)
}
