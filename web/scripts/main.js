const eventSource = new EventSource(window.location.origin + ":8080/api.sse");

function apiRequestSimple(req) {
    var xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/" + req);
    xhr.send();
}

eventSource.addEventListener("reset", function(evt) {
	
});

document.getElementById('xxxx').onclick = function() {
    apiRequestSimple("xxxxx");
};
