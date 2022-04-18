CXXFLAGS += -D_FILE_OFFSET_BITS=64

WudCompress/WudCompress: WudCompress/main.o WudCompress/wud.o
	$(CXX) $(LDFLAGS) -o $@ $^

install: WudCompress/WudCompress
	install -d $(DESTDIR)/bin/
	install WudCompress/WudCompress $(DESTDIR)/bin/

clean:
	rm -f WudCompress/WudCompress WudCompress/main.o WudCompress/wud.o

.PHONY: install
