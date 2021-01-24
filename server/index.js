const app = require('express')();
const http = require('http').Server(app);
const io = require('socket.io')(http);
const port = process.env.PORT || 8111;

app.get('/', (req, res) => {
  console.log("page");	 
  res.sendFile(__dirname + '/index.html');
});

io.on('connection', (socket) => {
  console.log("connect");	
  socket.on('msg', msg => {
    io.emit('msg', msg);
  });
  socket.on('ack', msg => {
    io.emit('ack', msg);
  });
});

http.listen(port, () => {
  console.log(`Socket.IO server running at http://localhost:${port}/`);
});
