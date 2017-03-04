package main

/*
#cgo CFLAGS: -I../gst -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/libsoup-2.4 -I/usr/include/libxml2 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/uuid -I/usr/include/gupnp-1.0 -I/usr/include/gssdp-1.0 -I/usr/include/libsoup-2.4 -I/usr/include/libxml2 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/uuid -I/usr/include/gupnp-av-1.0 -I/usr/include/gupnp-1.0 -I/usr/include/gssdp-1.0 -pthread -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/gssdp-1.0 -I/usr/include/libxml2 -I/usr/lib/x86_64-linux-gnu/gstreamer-1.0/include/
#cgo LDFLAGS: -L/home/vikram/go/src/httpMediaLiveServer/gst/ -L/usr/lib/x86_64-linux-gnu -ltarget -lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgupnp-1.0 -lgupnp-av-1.0 -lgssdp-1.0 -lxml2 -lpthread -lm -lz -licui18n -licuuc -licudata -llzma
#include <GstSource.h>
#include <Upnp.h>
#include <stdlib.h>
*/
import "C"
import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/textproto"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"
)

type state int

const (
	DOWN state = 0 + iota
	READY
	INIT
	RUN
)

func (s state) toString() string {
	switch s {
	case DOWN:
		return "down"

	case READY:
		return "ready"

	case INIT:
		return "init"

	case RUN:
		return "run"
	}

	return ""
}

type dmr struct {
	Name string `json:"name"`
	Usn  string `json:"usn"`
}

type dmrs struct {
	Len  int   `json:"len"`
	Dmrs []dmr `json:"dmrs"`
}

type storeS struct {
	status   state
	device   string
	pipeline *C.struct_GstSource
	then     time.Time
}

func (f *storeS) Read(p []byte) (int, error) {
	read := int(C.getData(f.pipeline, (*C.char)(unsafe.Pointer(&p[0])), C.int(len(p))))
	return read, nil
}

func (f *storeS) Seek(offset int64, whence int) (int64, error) {
	var crazyValue int64
	crazyValue = (1 << 31)

	if whence == 2 {
		return crazyValue, nil
	} else if whence == 1 {
		return 0, nil
	} else {
		return 0, nil
	}
}

type db map[string]*storeS

var mutex sync.Mutex

func (s *db) lock() {
	mutex.Lock()
}

func (s *db) unlock() {
	mutex.Unlock()
}

var vid int
var store db
var devices map[string]state
var hostIP string

func getDMRs(w http.ResponseWriter, r *http.Request) {
	var ds dmrs
	var count C.int
	var cds *C.struct_Renderer
	var cdx C.struct_Renderer

	dms := make(map[string]bool)
	cds = C.up_scan(&count)

	store.lock()

	for i := 0; i < int(count); i++ {
		cdx = *(*C.struct_Renderer)(unsafe.Pointer((uintptr(unsafe.Pointer(cds)) + uintptr(i)*unsafe.Sizeof(cdx))))
		ds.Dmrs = append(ds.Dmrs, dmr{C.GoString(&cdx.Name[0]), C.GoString(&cdx.Udn[0])})
		dms[C.GoString(&cdx.Udn[0])] = true
		if devices[C.GoString(&cdx.Udn[0])] == DOWN {
			devices[C.GoString(&cdx.Udn[0])] = READY
		}
	}

	for key := range devices {
		if dms[key] != true {
			devices[key] = DOWN
		}
	}

	ds.Len = int(count)

	store.unlock()

	C.free(unsafe.Pointer(cds))

	if w != nil {
		w.WriteHeader(200)
		w.Header().Add("Server", "A Go based HttpLiveMediaServer")
		w.Header().Set("Content-Type", "application/json")

		jData, _ := json.Marshal(ds)
		w.Write(jData)
	}
}

func getStatus(id string) state {
	store.lock()
	defer store.unlock()

	if store[id] == nil {
		return DOWN
	}

	return store[id].status
}

func setInit(device string, endpoint string) string {
	var ret C.int

	store.lock()
	defer store.unlock()

	vid++
	id := strconv.Itoa(vid)

	if store[id] != nil {
		return ""
	}

	devices[device] = INIT
	store[id] = &storeS{
		INIT,
		device,
		nil,
		time.Now(),
	}

	http.HandleFunc("/"+endpoint+id+".mp4", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "HEAD" {
			w.Header().Set("Pragma", "no-cache")
			w.Header().Set("Cache-control", "no-cache")
			w.Header().Set("Accept-Ranges", "none")
			w.Header().Add("contentFeatures.dlna.org",
				"DLNA.ORG_PN=AVC_MP4_BL_CIF15_AAC_520;DLNA.ORG_OP=00;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=05700000000000000000000000000000")
			w.Header().Add("transferMode.dlna.org", "Streaming")
			w.Header().Set("Content-Type", "video/mp4")
			//TESTVV
			fmt.Println("GOT HEAD!!!!!!")
			//w.Header().Add("EXT", "")
			//w.Header().Set("User-Agent", "HttpMediaServer/1.0")
			w.WriteHeader(200)
		} else if r.Method == "GET" {
			go func(c <-chan bool) {
				<-c
				setInactive(id)
			}(w.(http.CloseNotifier).CloseNotify())

			go func() {
				dur := time.Since(store[id].then)
				if dur > 10*time.Second {
					//TESTVV
					//setInactive(id)
				}
			}()

			w.Header().Set("Pragma", "no-cache")
			w.Header().Set("Cache-control", "no-cache")
			w.Header().Set("Accept-Ranges", "none")
			w.Header().Add("contentFeatures.dlna.org",
				"DLNA.ORG_PN=AVC_MP4_BL_CIF15_AAC_520;DLNA.ORG_OP=00;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=05700000000000000000000000000000")
			w.Header().Add("transferMode.dlna.org", "Streaming")
			w.Header().Set("Content-Type", "video/mp4")
			//TESTVV
			fmt.Println("Got GETTT")
			//w.Header().Add("EXT", "")
			//w.Header().Set("User-Agent", "HttpMediaServer/1.0")
			http.ServeContent(w, r, id+"_"+endpoint+"_"+device, time.Now(), store[id])
		}
	})

	store[id].pipeline = C.startPipeline(C.int(vid), C.CString(device), C.CString(endpoint),
		C.CString("http://"+hostIP+":7070/"+endpoint+id+".mp4"), &ret)
	if ret == -1 {
		fmt.Println("ERROR: failed to setup the pipeline")
		C.destroyPipeline(store[id].pipeline)
		return ""
	}

	return id
}

func setActive(id string) bool {
	store.lock()
	defer store.unlock()

	if store[id] == nil {
		return false
	}

	devices[store[id].device] = RUN
	store[id].status = RUN

	return true
}

func setInactive(id string) bool {
	store.lock()
	defer store.unlock()

	if store[id] == nil {
		return false
	}

	C.destroyPipeline(store[id].pipeline)
	devices[store[id].device] = READY
	store[id] = nil

	return true
}

func isDeviceAvailable(device string) bool {
	store.lock()
	defer store.unlock()

	return !(devices[device] == DOWN || devices[device] > READY)
}

func stream(w http.ResponseWriter, r *http.Request) {
	var code = 400
	var device, idv, action, endpoint string

	params := r.URL.Query()
	if params["device"] != nil {
		device = params["device"][0]
	}
	if params["endpoint"] != nil {
		endpoint = params["endpoint"][0]
	}
	if params["id"] != nil {
		idv = params["id"][0]
	}
	if params["action"] != nil {
		action = params["action"][0]
	}

	if r.Method != "POST" {
		code = 501
		goto end
	}

	if action == "stop" {
		if idv != "" {
			setInactive(idv)
		}
	} else if action == "play" {
		if device == "" || (endpoint != "camera") {
			goto end
		}

		if !isDeviceAvailable(device) {
			code = 503
		} else {
			if id := setInit(device, endpoint); id == "" {
				setInactive(id)
				code = 503
			} else {
				w.Header().Add("Identifier", id)
				w.Header().Add("Healthport", "3221")
				code = 200
			}
		}
	}

end:
	w.WriteHeader(code)
}

func monitorStreams(ln net.Listener) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			//TODO
		} else {
			tConn := conn.(*net.TCPConn)
			tConn.SetKeepAlive(true)

			go func(cn net.Conn) {
				for {
					st := DOWN
					reader := bufio.NewReader(cn)
					rp := textproto.NewReader(reader)

					line, err := rp.ReadLine()
					if err != nil {
						break
					}

					s := strings.Split(line, ":")
					if len(s) == 2 && s[0] == "status" {
						st = getStatus(s[1])
						store[s[1]].then = time.Now()
					}

					tConn.Write([]byte(st.toString() + "\r\n"))
				}
			}(conn)
		}
	}
}

func checkNetworkInterface(str string) error {
	ifaces, err := net.Interfaces()
	if err != nil {
		return err
	}

	for _, iface := range ifaces {
		if iface.Name != str {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			switch t := addr.(type) {
			case *net.IPNet:
				if t.IP.To4() != nil {
					hostIP = t.IP.String()
					return nil
				}
			}
		}
	}

	return errors.New("Interface " + str + " not found or does not have an IPv4 address")
}

func main() {
	vid = 9235
	store = make(db)
	devices = make(map[string]state)

	if err := checkNetworkInterface(os.Args[1]); err != nil {
		fmt.Println(err)
		os.Exit(-1)
	}

	fmt.Println("Stream IP: " + hostIP)
	http.HandleFunc("/dmrs", getDMRs)
	http.HandleFunc("/stream", stream)

	if ln, err := net.Listen("tcp", ":3221"); err != nil {
		fmt.Println(err)
		os.Exit(-1)
	} else {
		go monitorStreams(ln)
	}

	C.start_upnp()
	getDMRs(nil, nil)

	if err := http.ListenAndServe(":7070", nil); err != nil {
		fmt.Println(err)
		os.Exit(-1)
	}
}
