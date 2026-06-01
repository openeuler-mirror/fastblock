package exporterclient

import "errors"

var ErrNotImplemented = errors.New("exporter client not implemented")
var ErrNotFound = errors.New("export not found")
var ErrIncomplete = errors.New("export is incomplete")
