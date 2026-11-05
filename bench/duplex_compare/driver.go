// SPDX-License-Identifier: Apache-2.0
package main

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"sync"
	"time"
)

// Each reader has one bounded buffer and a one-record channel. No capture grows.
func reader(ctx context.Context, stream io.Reader, expected []byte, requests, width int, start <-chan struct{}, result chan<- error, group *sync.WaitGroup) {
	defer group.Done()
	var buffer [257]byte
	send := func(err error) bool {
		select {
		case result <- err:
			return true
		case <-ctx.Done():
			return false
		}
	}
	for request := 0; request < requests; request++ {
		select {
		case <-start:
		case <-ctx.Done():
			return
		}
		for offset := 0; offset < len(expected); {
			n, err := stream.Read(buffer[:width])
			if err != nil {
				send(err)
				return
			}
			if n <= 0 || n > len(expected)-offset {
				send(fmt.Errorf("zero progress or per-request excess output"))
				return
			}
			if !bytes.Equal(buffer[:n], expected[offset:offset+n]) {
				send(fmt.Errorf("output oracle"))
				return
			}
			offset += n
		}
		if !send(nil) {
			return
		}
	}
	var tail [1]byte
	n, err := stream.Read(tail[:])
	if n != 0 || err != io.EOF {
		send(fmt.Errorf("excess output or missing EOF: %d %v", n, err))
		return
	}
	send(nil)
}

func execute(worker string, frame []byte, requests int) (time.Duration, error) {
	began := time.Now()
	var group sync.WaitGroup
	outResult, errResult := make(chan error, 1), make(chan error, 1)
	outStart, errStart := make(chan struct{}), make(chan struct{})
	ack := []byte{'a', 'c', 'k', 0}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, worker)
	input, err := cmd.StdinPipe()
	if err != nil {
		return 0, err
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		input.Close()
		return 0, err
	}
	errors, err := cmd.StderrPipe()
	if err != nil {
		input.Close()
		output.Close()
		return 0, err
	}
	if err = cmd.Start(); err != nil {
		input.Close()
		output.Close()
		errors.Close()
		return 0, err
	}
	waited := false
	defer func() {
		cancel()
		if !waited {
			cmd.Process.Kill()
			cmd.Wait()
		}
		input.Close()
		output.Close()
		errors.Close()
		group.Wait()
	}()
	group.Add(2)
	go reader(ctx, output, frame, requests, 257, outStart, outResult, &group)
	go reader(ctx, errors, ack, requests, 3, errStart, errResult, &group)
	receive := func(channel <-chan error) error {
		select {
		case err := <-channel:
			return err
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	for request := 0; request < requests; request++ {
		for _, start := range []chan struct{}{outStart, errStart} {
			select {
			case start <- struct{}{}:
			case <-ctx.Done():
				return 0, ctx.Err()
			}
		}
		for offset := 0; offset < len(frame); {
			n, err := input.Write(frame[offset:])
			if err != nil {
				return 0, err
			}
			if n <= 0 {
				return 0, fmt.Errorf("zero stdin progress")
			}
			offset += n
		}
		if err = receive(outResult); err != nil {
			return 0, err
		}
		if err = receive(errResult); err != nil {
			return 0, err
		}
	}
	if err = input.Close(); err != nil {
		return 0, err
	}
	if err = receive(outResult); err != nil {
		return 0, err
	}
	if err = receive(errResult); err != nil {
		return 0, err
	}
	err = cmd.Wait()
	waited = true
	if err != nil {
		return 0, err
	}
	group.Wait()
	cancel()
	output.Close()
	errors.Close()
	return time.Since(began), nil
}

func main() {
	if len(os.Args) != 4 || !filepath.IsAbs(os.Args[1]) {
		panic("absolute worker, size and request count required")
	}
	size, err := strconv.Atoi(os.Args[2])
	if err != nil || (size != 31 && size != 4096) {
		panic("unsupported payload size")
	}
	requests, err := strconv.Atoi(os.Args[3])
	if err != nil || requests != 128 {
		panic("unsupported request count")
	}
	frame := make([]byte, size+4)
	frame[2] = byte(size / 256)
	frame[3] = byte(size % 256)
	for i := 0; i < size; i++ {
		frame[i+4] = byte((i*17 + i/251) % 256)
	}
	elapsed, err := execute(os.Args[1], frame, requests)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println(elapsed.Nanoseconds(), requests*len(frame), requests*4)
}
