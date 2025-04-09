build:
	clang -pthread server.c -o server
	clang -pthread client.c -o client