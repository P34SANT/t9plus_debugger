package main

import "testing"

func TestMatchingPorts(t *testing.T) {
	text := []byte("Port 01: device\n -> usbip://192.168.2.99:3240/1-1\n" +
		"Port 02: device\n -> usbip://192.168.2.99:3240/1-2\n" +
		"Port 03: device\n -> usbip://192.168.2.98:3240/1-1\n")
	got := matchingPorts(text)
	if len(got) != 1 || got[0] != 1 {
		t.Fatalf("incorrect device matching: %v", got)
	}
}

func TestMatchingPortsReportsDuplicates(t *testing.T) {
	text := []byte("Port 01: device\n -> usbip://192.168.2.99:3240/1-1\n" +
		"Port 04: device\n -> usbip://192.168.2.99:3240/1-1\n")
	if got := matchingPorts(text); len(got) != 2 {
		t.Fatalf("duplicates must remain visible: %v", got)
	}
}
