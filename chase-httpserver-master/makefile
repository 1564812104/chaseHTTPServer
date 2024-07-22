server:main.o http_conn.o
	g++ -pthread main.o http_conn.o -o server

%.o:%.c
	g++ -c $< -o $@

.PHONY:clean
clean:
	rm -f *.o server
