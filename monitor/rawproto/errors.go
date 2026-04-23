package rawproto

func ErrorStatus(err error) uint32 {
	if err == nil {
		return StatusOK
	}
	return StatusInternalError
}
