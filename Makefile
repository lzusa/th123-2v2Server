BINARY := 2v2Server
PKG    := .
OUT    := bin

.PHONY: all build linux test race vet fmt clean

all: build

build:
	go build -trimpath -o $(OUT)/$(BINARY) $(PKG)

linux:
	GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -o $(OUT)/$(BINARY)-linux-amd64 $(PKG)
	GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -trimpath -o $(OUT)/$(BINARY)-linux-arm64 $(PKG)

test:
	go test ./...

# -race 需要 cgo（也就是需要 gcc），在没有 C 工具链的机器上会失败
race:
	CGO_ENABLED=1 go test -race ./...

vet:
	go vet ./...

fmt:
	gofmt -w .

clean:
	rm -rf $(OUT)
