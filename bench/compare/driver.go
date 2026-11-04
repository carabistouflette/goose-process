// SPDX-License-Identifier: Apache-2.0
// Standard-library capture baseline; no timeout or hard capture cap.
package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"time"
)

func main() {
	fixture, mode := os.Args[1], os.Args[2]
	count, err := strconv.Atoi(os.Args[3])
	if err != nil {
		panic(err)
	}
	args := []string{mode}
	if mode == "args" {
		for i := 0; i < 128; i++ {
			args = append(args, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")
		}
	}
	var input, expected, errors []byte
	if mode == "input" {
		input = bytes.Repeat([]byte{'x'}, 131072)
		expected = input
	}
	if mode == "streams" || mode == "redirect" {
		expected = bytes.Repeat([]byte{'x'}, 65536)
	}
	if mode == "streams" {
		errors = bytes.Repeat([]byte{'y'}, 65536)
	}
	invoke := func() {
		cmd := exec.Command(fixture, args...)
		var out, stderr bytes.Buffer
		cmd.Stdout = &out
		if mode != "redirect" {
			cmd.Stderr = &stderr
		}
		if mode == "input" {
			cmd.Stdin = bytes.NewReader(input)
		}
		if mode == "environment" {
			cmd.Env = []string{"PROBE=goose"}
		}
		if err := cmd.Run(); err != nil {
			panic(err)
		}
		if !bytes.Equal(out.Bytes(), expected) || !bytes.Equal(stderr.Bytes(), errors) {
			panic("output mismatch")
		}
	}
	for i := 0; i < 3; i++ {
		invoke()
	}
	start := time.Now()
	for i := 0; i < count; i++ {
		invoke()
	}
	fmt.Println(time.Since(start).Nanoseconds() / int64(count))
}
