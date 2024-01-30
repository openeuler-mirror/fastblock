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
package log

import (
	"context"
	"fmt"
	"io"
	"log"
	"os"
	"runtime"
	"strings"
)

type level int

// Error level.
const (
	ErrorLevel level = 0 // Errors should be properly handled
	WarnLevel  level = 1 // Errors could be ignored; messages might need noticed
	InfoLevel  level = 2 // Informational messages
	DebugLevel level = 3 // Debug only.
)

func parseLevel(levelString string) level {
	switch strings.ToLower(levelString) {
	case "debug":
		return DebugLevel
	case "info":
		return InfoLevel
	case "warn":
		return WarnLevel
	case "error":
		return ErrorLevel
	default:
		return InfoLevel
	}
}

// Logger Wrapper.
type Logger struct {
	out    io.WriteCloser
	level  level
	logger *log.Logger
}

var logger *Logger

var logFlags = log.Ldate | log.Ltime | log.Lmicroseconds

// NewFileLogger create a new file logger.
func NewFileLogger(path string, logLevel string) {
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE|os.O_APPEND, 0666)
	if err != nil {
		panic("Failed to open log file " + path)
	}

	logger = &Logger{
		out:    f,
		level:  parseLevel(logLevel),
		logger: log.New(f, "", logFlags),
	}
}

func getCaller(skipCallDepth int) string {
	_, fullPath, line, ok := runtime.Caller(skipCallDepth)
	if !ok {
		return ""
	}
	fileParts := strings.Split(fullPath, "/")
	file := fileParts[len(fileParts)-1]
	return fmt.Sprintf("%s:%d", file, line)
}

func (l *Logger) prefixArray() []interface{} {
	array := make([]interface{}, 0, 3)
	array = append(array, getCaller(4))

	return array
}

// Debug for debugging only and should not be enabled in field. It may generate a lot of log.
func Debug(ctx context.Context, args ...interface{}) {
	if logger == nil {
		return
	}
	logger.logInternal(ctx, DebugLevel, "[DEBUG]", args)
}

// Info for lowest level trace.
func Info(ctx context.Context, args ...interface{}) {
	if logger == nil {
		return
	}
	logger.logInternal(ctx, InfoLevel, "[INFO]", args)
}

// Warn for something wrong.
func Warn(ctx context.Context, args ...interface{}) {
	if logger == nil {
		return
	}
	logger.logInternal(ctx, WarnLevel, "[WARN]", args)
}

// Error for serious error.
func Error(ctx context.Context, args ...interface{}) {
	if logger == nil {
		return
	}
	logger.logInternal(ctx, ErrorLevel, "[ERROR]", args)
}

func (l *Logger) logInternal(ctx context.Context, logLevel level, prefix string, args ...interface{}) {
	if l.level < logLevel {
		return
	}
	prefixArray := l.prefixArray()
	prefixArray = append(prefixArray, prefix)
	args = append(prefixArray, args...)
	l.logger.Println(args...)
}

// Close the file.
func Close() error {
	if logger == nil {
		return nil
	}
	return logger.out.Close()
}