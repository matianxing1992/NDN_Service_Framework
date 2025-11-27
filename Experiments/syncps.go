package main

import (
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	spec_repo "github.com/named-data/ndnd/repo/tlv"
	enc "github.com/named-data/ndnd/std/encoding"
	"github.com/named-data/ndnd/std/engine"
	"github.com/named-data/ndnd/std/log"
	"github.com/named-data/ndnd/std/ndn"
	spec "github.com/named-data/ndnd/std/ndn/spec_2022"
	"github.com/named-data/ndnd/std/ndn/svs_ps"
	"github.com/named-data/ndnd/std/object"
	"github.com/named-data/ndnd/std/object/storage"
	ndn_sync "github.com/named-data/ndnd/std/sync"
)

var group, _ = enc.NameFromStr("/muas")
var name enc.Name
var svsalo *ndn_sync.SvsALO
var store ndn.Store
var client ndn.Client
var repoName, _ = enc.NameFromStr("/ndnd/repo")

const SnapshotThreshold = 100

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: %s <name>\n", os.Args[0])
		os.Exit(1)
	}

	// 解析节点名
	var err error
	name, err = enc.NameFromStr(os.Args[1])
	if err != nil {
		log.Fatal(nil, "Invalid node ID", "name", os.Args[1])
		return
	}

	// 创建引擎
	app := engine.NewBasicEngine(engine.NewDefaultFace())
	err = app.Start()
	if err != nil {
		log.Fatal(nil, "Unable to start engine", "err", err)
		return
	}
	defer app.Stop()

	// 持久化对象存储
	ident := strings.ReplaceAll(name.String(), "/", "-")
	bstore, err := storage.NewBadgerStore(fmt.Sprintf("db-chat%s", ident))
	if err != nil {
		log.Error(nil, "Unable to create object store", "err", err)
		return
	}
	defer bstore.Close()
	store = bstore

	// 对象客户端
	client = object.NewClient(app, store, nil)
	if err = client.Start(); err != nil {
		log.Error(nil, "Unable to start object client", "err", err)
		return
	}
	defer client.Stop()

	// 初始化 SVS ALO
	svsalo, err = ndn_sync.NewSvsALO(ndn_sync.SvsAloOpts{
		Name:         name,
		InitialState: readState(),
		Svs: ndn_sync.SvSyncOpts{
			Client:      client,
			GroupPrefix: group,
		},
		Snapshot: &ndn_sync.SnapshotNodeHistory{
			Client:    client,
			Threshold: SnapshotThreshold,
		},
	})
	if err != nil {
		panic(err)
	}

	svsalo.SetOnError(func(err error) {
		fmt.Fprintf(os.Stderr, "*** %v\n", err)
	})

	// ============ 订阅所有发布者 ============
	svsalo.SubscribePublisher(enc.Name{}, func(pub ndn_sync.SvsPub) {
		now := time.Now().Format("15:04:05.000") // 毫秒级时间
		if !pub.IsSnapshot {
			fmt.Printf("[recv %s] %s: %s\n", now, pub.Publisher, pub.Bytes())
		} else {
			snapshot, err := svs_ps.ParseHistorySnap(enc.NewWireView(pub.Content), true)
			if err != nil {
				panic(err)
			}
			fmt.Fprintf(os.Stderr, "[recv %s] *** Snapshot from %s with %d entries\n",
				now, pub.Publisher, len(snapshot.Entries))
			for _, entry := range snapshot.Entries {
				fmt.Printf("[recv %s] %s: %s\n", now, pub.Publisher, entry.Content.Join())
			}
		}
		commitState(pub.State)
	})

	// 宣告前缀
	for _, route := range []enc.Name{
		svsalo.SyncPrefix(),
		svsalo.DataPrefix(),
	} {
		client.AnnouncePrefix(ndn.Announcement{Name: route})
		defer client.WithdrawPrefix(route, nil)
	}

	if err = svsalo.Start(); err != nil {
		log.Error(nil, "Unable to start SVS ALO", "err", err)
		return
	}
	defer svsalo.Stop()

	client.ExpressCommand(
		repoName.Append(enc.NewKeywordComponent("cmd")),
		name.Append(enc.NewKeywordComponent("repo")),
		(&spec_repo.RepoCmd{
			SyncJoin: &spec_repo.SyncJoin{
				Protocol: &spec.NameContainer{Name: spec_repo.SyncProtocolSvsV3},
				Group:    &spec.NameContainer{Name: group},
				HistorySnapshot: &spec_repo.HistorySnapshotConfig{
					Threshold: SnapshotThreshold,
				},
			},
		}).Encode(),
		func(w enc.Wire, err error) {
			if err != nil {
				log.Warn(nil, "Repo sync join command failed", "err", err)
			} else {
				log.Info(nil, "Repo joined SVS group")
			}
		})

	fmt.Fprintln(os.Stderr, "*** Joined SVS ALO chat group")
	fmt.Fprintln(os.Stderr, "*** You are:", name)
	fmt.Fprintln(os.Stderr, "*** Auto-publishing every 6ms. Press Ctrl+C to exit.\n")

	// 上线公告
	publish([]byte("Entered the chat room"))

	// === 自动发布循环 ===
	ticker := time.NewTicker(6 * time.Millisecond)
	defer ticker.Stop()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, os.Interrupt, syscall.SIGTERM)

	counter := 1
	for {
		select {
		case <-ticker.C:
			payload := []byte(fmt.Sprintf("Message %d", counter))
			publish(payload)
			counter++
		case <-quit:
			fmt.Fprintln(os.Stderr, "\n*** Stopping auto publisher...")
			return
		}
	}
}

// publish 带时间打印
func publish(content []byte) {
	sendTime := time.Now().Format("15:04:05.000")
	fmt.Printf("[send %s] %s\n", sendTime, string(content))

	_, state, err := svsalo.Publish(enc.Wire{content})
	if err != nil {
		log.Error(nil, "Unable to publish message", "err", err)
	}
	commitState(state)
}

func publishBlob(content []byte) {
	blobName := svsalo.DataPrefix().
		Append(enc.NewKeywordComponent("blob")).
		WithVersion(enc.VersionUnixMicro)

	verName, err := client.Produce(ndn.ProduceArgs{
		Name:    blobName,
		Content: enc.Wire{content},
	})
	if err != nil {
		log.Error(nil, "Unable to publish blob", "err", err)
		return
	}

	cmd := spec_repo.RepoCmd{
		BlobFetch: &spec_repo.BlobFetch{
			Name: &spec.NameContainer{Name: verName},
		},
	}
	publish(cmd.Encode().Join())
}

func commitState(state enc.Wire) {
	store.Put(group, state.Join())
}

func readState() enc.Wire {
	stateWire, err := store.Get(group, false)
	if err != nil {
		panic("unable to get state (store is broken)")
	}
	if stateWire == nil {
		return nil
	}
	return enc.Wire{stateWire}
}
