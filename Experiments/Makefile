APP=syncps
BIN=/usr/local/bin/$(APP)

all: build

build:
	go build -o $(APP) syncps.go

install: build
	sudo install -m 755 ./$(APP) $(BIN)

run:
	$(APP) /muas/test

clean:
	rm -f $(APP)
