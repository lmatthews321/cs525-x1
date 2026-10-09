## Predict

#### The minimum connect time from your laptop to each of the five mirrors in the table below, computed from distance alone: the great circle distance there and back at the speed of light in fiber, 2 x 10^8 m/s (200 km per millisecond).

Calculation for the Min Connect Time = 2 X Great Circle Distance / speed of light in fiber

|Location|Great Circle Distance (1 way)|Predicted Min Connect Time
|Salt Lake City, UT|476 km|4.76 ms
|Berkeley, CA|808 km|8.08 ms
|Rochester, NY|3090 km|30.9 ms
|Erlangen, Germany|8380 km|83.8 ms
|Nomi, Japan|8240 km|82.4 ms

#### Which mirror will have the longest connect time from the Codespace, and why.

Germany should have the longest connect time based on distance. My current Codespace region is westus2 wich is centered in the Seatle area. This makes Germany even further away than the above table indicates. However, depending on what hops are taken Japan could end up being longer. 

#### Whether the DNS time changes between trial 1 and trial 2, and why.

The DNS time may decrease between the first and second trial. This is because the first request may have to travel to the root or Top-level domain DNS server, or more likely the authoritative server to resolve the request. After this request, the local DNS server will have the informatin in its cache which will significantly decrease the resolution time for the second request.

#### Which of your three vantage points are behind a NAT.

All three vantage points should be behind a NAT. I know for sure my laptop and Onyx are because I know they have private IP addresses. Without checking the Codespace IP, I'm sure they must use one too so they can create multiple Codespaces for all the users without using up all their puplic IP addresses.

#### Whether downloading the same 13 MB file from Utah and from Japan will take about the same time, and why.

Downloading the 13 MB file from Japan should take much longer. The propagation delay is much longer due to the distance packets must travel. There are also probalby more hops involved in the route to Japan would incur additional processing, queueing, and transmission delays


