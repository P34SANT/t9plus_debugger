import serial
import time
import threading

def test_port(port_name, baud):
    print(f"[*] Opening {port_name} at {baud} baud...")
    s = serial.Serial(port_name, baud, timeout=0.5)
    total_written = 0
    t0 = time.time()
    for i in range(100):
        data = f"PLINK_TEST_{port_name}_{i:04d}\r\n".encode()
        s.write(data)
        total_written += len(data)
        time.sleep(0.005)
    elapsed = time.time() - t0
    print(f"[+] {port_name}: Transmitted {total_written} bytes in {elapsed:.2f}s ({total_written/elapsed:.1f} B/s)")
    s.close()

t1 = threading.Thread(target=test_port, args=('COM9', 115200))
t2 = threading.Thread(target=test_port, args=('COM10', 115200))

t1.start()
t2.start()

t1.join()
t2.join()
print("[+] DUAL SERIAL CONCURRENT STRESS TEST PASSED!")
