/*
	Copyright (C) 2016 Milan Bielik <milan.bielik@post.cz>
	
	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*

Panasonic TV UPnP service control v1.0
--------------------------------------

Compile:
gcc PanaTV.c -o PanaTV

TVheadend patch:
src/muxer.c
-   { "video/mp2t",               MC_MPEGTS },
+   { "video/mpeg",               MC_MPEGTS },

Some examples:
192.168.168.209 - Panasonic TV TCP/IP address
55000 - Panasonic TV UPnP TCP/IP port
192.168.168.201 - TVheadend TCP/IP address for live streaming to TV

./PanaTV 192.168.168.209 55000 key CH_UP   ;channel up

./PanaTV 192.168.168.209 55000 st
./PanaTV 192.168.168.209 55000 set 'http://192.168.168.201:9981/stream/channelname/CT%201%20HD'
./PanaTV 192.168.168.209 55000 pl		;play stream from TVheadend

./PanaTV 192.168.168.209 55000 event 2222    ;event & heartbeat(every min) notification TCP/IP port
./PanaTV 192.168.168.209 55000 regevent dmr0 192.168.168.201 2222  ;register TV UPnP event 
./PanaTV 192.168.168.209 55000 regevent nrc0 192.168.168.201 2222

./PanaTV 192.168.168.209 55000 applist			;get application id
./PanaTV 192.168.168.209 55000 launchapp product_id=0387878700000032	;run application on TV

./PanaTV 192.168.168.209 55000 dial GET YouTube			;read status
./PanaTV 192.168.168.209 55000 dial POST YouTube param	;run YouTube throught DIAL protocol on TV
./PanaTV 192.168.168.209 55000 dial DELETE YouTube/run	;exit YouTube


TV UPnP configs:
wget http://192.168.168.209:55000/nrc/ddd.xml
wget http://192.168.168.209:55000/nrc/sdd_0.xml

wget http://192.168.168.209:55000/pac/ddd.xml
wget http://192.168.168.209:55000/pac/sdd_0.xml

wget http://192.168.168.209:55000/dms/ddd.xml
wget http://192.168.168.209:55000/dms/sdd_0.xml
wget http://192.168.168.209:55000/dms/sdd_1.xml

wget http://192.168.168.209:55000/dmr/ddd.xml
wget http://192.168.168.209:55000/dmr/sdd_0.xml
wget http://192.168.168.209:55000/dmr/sdd_1.xml
wget http://192.168.168.209:55000/dmr/sdd_2.xml


Docs:
http://openconnectivity.org/upnp/specifications/mediaserver1-and-mediarenderer1
http://www.dial-multiscreen.org/dial-protocol-specification

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <time.h>

#define EPORT 55200
#define TMOUT 290
#define MAXBUFF 16*1024
#define N(x) sizeof(x)/sizeof(x[0])

struct hostent *tvhst;
int tvport=0;
char *tvaddr=NULL;

char *keys[]={"CH_DOWN","CH_UP","VOLUP","VOLDOWN","MUTE","TV","CHG_INPUT","POWER","RED","GREEN","YELLOW","BLUE",
"CANCEL","SUBMENU","RETURN","ENTER","RIGHT","LEFT","UP","DOWN","MENU","EPG","TEXT","STTL","INFO","HOLD","R_TUNE",
"REW","PLAY","FF","SKIP","PAUSE","STOP","REC","D0..D9","HDMI1","HDMI2","HDMI3","APPS",
"AD_CHANGE","CHAT_MODE","DISP_MODE","DMS_CH_DOWN","DMS_CH_UP","GUIDE","HOME","INDEX","INTERNET","NETFLIX","MIC_NRC","MPX","MYBUTTON","OFFTIMER","OSD_REMOTE","PICTAI",
"P_NR","SD_CARD","SKIP_NEXT","SKIP_PREV","SURROUND","SWAP","TV_MUTE_ON","TV_MUTE_OFF","VIERA_LINK","VIDEO1","VTOOLS"};

struct {
	char *url;
	char *sch;
	char sid[40];
	int tm;
	} sch[]={
	{"pac0","panasonic-com:service:p00ProAVControlService:1",""},
	{"nrc0","panasonic-com:service:p00NetworkControl:1",""},
	{"dms0","schemas-upnp-org:service:ContentDirectory:1",""},
	{"dms1","schemas-upnp-org:service:ConnectionManager:1",""},
	{"dmr0","schemas-upnp-org:service:RenderingControl:1",""},
	{"dmr1","schemas-upnp-org:service:ConnectionManager:1",""},
	{"dmr2","schemas-upnp-org:service:AVTransport:1",""}};

struct {
	char *cmd;
	char *url;
	char *fn;
	char *par;
	int val;
	} cmd[]={
	{"set","dmr2","SetAVTransportURI","<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>",1},
	{"pl","dmr2","Play","<InstanceID>0</InstanceID><Speed>1</Speed>",0},
	{"pa","dmr2","Pause","<InstanceID>0</InstanceID>",0},
	{"st","dmr2","Stop","<InstanceID>0</InstanceID>",0},
	{"getmedia","dmr2","GetMediaInfo","<InstanceID>0</InstanceID>",0},
	{"gettrans","dmr2","GetTransportInfo","<InstanceID>0</InstanceID>",0},
	{"getposi","dmr2","GetPositionInfo","<InstanceID>0</InstanceID>",0},
	{"getdev","dmr2","GetDeviceCapabilities","<InstanceID>0</InstanceID>",0},
	{"getbyte","dmr2","X_DLNA_GetBytePositionInfo","<InstanceID>0</InstanceID>",0},

	{"key","nrc0","X_SendKey","<X_KeyEvent>NRC_%s-ONOFF</X_KeyEvent>",1},
	{"string","nrc0","X_SendString","<X_String>%s</X_String>",1},
	{"getinput","nrc0","X_GetInputMode","",0},
	{"getkey","nrc0","X_GetKeyboardType","",0},
	{"getevent1","nrc0","X_GetEventServerUrl","",0},
	{"getevent2","dmr0","X_GetEventServerUrl","",0},
	{"getevent3","dms0","X_GetEventServerUrl","",0},
	{"getvect","nrc0","X_GetVectorInfo","",0},
	{"getgame","nrc0","X_GetGamepadInfo","",0},
	{"getvoice","nrc0","X_GetVoiceCtrlInfo","",0},
	{"getapp","nrc0","X_GetAppInfo","<X_InfoType>vc_app</X_InfoType>",0},
	{"getapplist","nrc0","X_GetAppInfo","<X_InfoType>vc_app_list</X_InfoType>",0},
	{"applist","nrc0","X_GetAppList","",0},
	{"queryapp","nrc0","X_QueryApp","<X_AppType>vc_app</X_AppType><X_SessionId>1</X_SessionId><X_QueryKeyword>%s</X_QueryKeyword>",1},
	{"launchapp","nrc0","X_LaunchApp","<X_AppType>vc_app</X_AppType><X_LaunchKeyword>%s</X_LaunchKeyword>",1},

	{"getvol","dmr0","GetVolume","<InstanceID>0</InstanceID><Channel>Master</Channel>",0},
	{"setvol","dmr0","SetVolume","<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>%s</DesiredVolume>",1},
	{"getmute","dmr0","GetMute","<InstanceID>0</InstanceID><Channel>Master</Channel>",0},
	{"setmute","dmr0","SetMute","<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredMute>%s</DesiredMute>",1},
	{"getprinfo","dmr1","GetProtocolInfo","",0},
	{"getconid","dmr1","GetCurrentConnectionIDs","",0},
	{"getconinfo","dmr1","GetCurrentConnectionInfo","<ConnectionID>%s</ConnectionID>",1},

	{"drivelist","dms0","X_DriveList","<Filter>*</Filter><StartingIndex>0</StartingIndex><RequestedCount>0</RequestedCount>",0},
	{"browmeta","dms0","Browse","<ObjectID>%s</ObjectID><BrowseFlag>BrowseMetadata</BrowseFlag><Filter>*</Filter><StartingIndex>0</StartingIndex><RequestedCount>0</RequestedCount><SortCriteria></SortCriteria>",1},
	{"browdir","dms0","Browse","<ObjectID>%s</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>*</Filter><StartingIndex>0</StartingIndex><RequestedCount>0</RequestedCount><SortCriteria></SortCriteria>",1},
	{"createobj","dms0","CreateObject","<ContainerID>0</ContainerID><Elements>%s</Elements>",1},
	{"getid","dms0","GetSystemUpdateID","",0},
	{"getxid","dms0","X_GetSystemUpdateID","",0},
	{"getsort","dms0","GetSortCapabilities","",0},
	{"getsearch","dms0","GetSearchCapabilities","",0},

	{"dial","xxxx","","",2},
	{"event","xxxx","","",1},
	{"regevent","xxxx","","",3}
	};

int sendtoTV(int port,int cont,char *head,char *data)
{
	struct sockaddr_in dest={0};
	int sockfd,i;
	char buff[MAXBUFF];

	sockfd=socket(AF_INET,SOCK_STREAM,0);
	dest.sin_family=AF_INET;
	dest.sin_port=htons(port);
	memcpy(&dest.sin_addr.s_addr,tvhst->h_addr,tvhst->h_length);
	connect(sockfd,(struct sockaddr*)&dest,sizeof(dest));

	if(head)
		send(sockfd,head,strlen(head),0);
	send(sockfd,data,strlen(data),0);
	if(cont)
		return sockfd;
	while((i=recv(sockfd,buff,sizeof(buff),0)) > 0)
	{
		buff[i]='\0';
		printf("%s",buff);
	}
	close(sockfd);
	
	return 0;
}

int req(int ind,char *argv[])
{
	int i,n;
	char head[MAXBUFF],data[MAXBUFF];

	for(i=0;i < N(sch);i++)
		if(!strcmp(sch[i].url,cmd[ind].url))
			break;
	if(i == N(sch))
	{
		printf("Invalid request !\n");
		return -1;
	}

	switch(cmd[ind].val)
	{
		case 0:
		strcpy(head,cmd[ind].par);
		break;
		case 1:
		sprintf(head,cmd[ind].par,argv[0]);
		break;
		case 2:
		sprintf(head,cmd[ind].par,argv[0],argv[1]);
		break;
	}
	n=sprintf(data,"<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
	"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
	"<s:Body><u:%s xmlns:u=\"urn:%s\">%s</u:%s></s:Body></s:Envelope>",cmd[ind].fn,sch[i].sch,head,cmd[ind].fn);

	sprintf(head,"POST /%.3s/control_%c HTTP/1.1\r\nUser-Agent: Panasonic VR-CP UPnP/2.0\r\nHost: %s:%i\r\nSOAPACTION: \"urn:%s#%s\"\r\nContent-Length: %i\r\n\r\n",
	cmd[ind].url,cmd[ind].url[3],tvaddr,tvport,sch[i].sch,cmd[ind].fn,n);

	printf("%s%s\n",head,data);
	sendtoTV(tvport,0,head,data);
	
	return 0;
}

int chport(char *p)
{
	char *lp;
	int i=strtol(p,&lp,10);
	
	if(i < 1 || i > 65535 || *lp)
	{
		printf("Invalid IP port !\n");
		return -1;
	}
	return i;
}

int main(int argc, char *argv[])
{
	int i;
	char data[MAXBUFF];
	
	if(argc < 4 )
	{
		printf("Usage : %s tv_addr tv_port command [option]\nVersion 1.0\nCommands:",argv[0]);
		for(i=0;i < N(cmd);i++)
			printf(" %s",cmd[i].cmd);
		printf("\n");
		return 1;
	}
	for(i=0;i < N(cmd);i++)
		if(!strcmp(cmd[i].cmd,argv[3]))
			break;
	if(i == N(cmd))
	{
		printf("Invalid command !\n");
		return 2;
	}
	if(argc-cmd[i].val < 4)
	{
		printf("Missing %i value !\n",4+cmd[i].val-argc);
		return 3;
	}
	if(!(tvhst=gethostbyname(argv[1])))
	{
		printf("Invalid TV IP address !\n");
		return 4;
	}
	else
		tvaddr=argv[1];
	if((tvport=chport(argv[2])) < 0)
		return 5;
	if(!strcmp(argv[3],"dial"))
	{
		i=argc > 6 ? strlen(argv[6]) : 0;
		sprintf(data,"%s /nrc/dial/%s HTTP/1.1\r\nContent-Length: %i\r\nHost: %s:%i\r\n\r\n%s",argv[4],argv[5],i,tvaddr,tvport,i ? argv[6] : "");
		printf("%s\n",data);
		sendtoTV(tvport,0,NULL,data);
	}
	else
	if(!strcmp(argv[3],"regevent"))
	{
		for(i=0;i < N(sch);i++)
			if(!strcmp(sch[i].url,argv[4]))
				break;
		if(i == N(sch))
		{
			printf("Invalid param !\n");
			return 6;
		}
		if((i=chport(argv[6])) < 0)
			return 7;

		sprintf(data,"SUBSCRIBE /%.3s/event_%c HTTP/1.1\r\nHost: %s:%i\r\nCALLBACK: <http://%s:%i/%s>\r\nNT: upnp:event\r\n\r\n",argv[4],argv[4][3],tvaddr,tvport,argv[5],i,argv[4]);
		printf("%s",data);
		sendtoTV(tvport,0,NULL,data);
	}
	else
	if(!strcmp(argv[3],"event"))
	{
		char hb[256],*lpsid,*lpurl,*end="</e:propertyset>";
		int sockl,sockcli,sockheart,j,tmout=-1,ep,lend=strlen(end);
		struct sockaddr_in client={0};
		struct epoll_event epp,event;
		
		client.sin_family=AF_INET;
		if((i=chport(argv[4])) < 0)
			return 8;

		printf("Listening on port %i\n",i);
		client.sin_port=htons(i);
		client.sin_addr.s_addr=INADDR_ANY;
		
		sprintf(hb,"GET /event HTTP/1.1\r\nHost: %s:%i\r\n\r\n",tvaddr,EPORT);
		printf("%s",hb);
			
		sockl=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
		i=1;
		setsockopt(sockl,SOL_SOCKET,SO_REUSEADDR,(char*)&i,sizeof(i));
		fcntl(sockl,F_SETFL,O_NONBLOCK);
		bind(sockl,(struct sockaddr*)&client,sizeof(client));
		listen(sockl,4);

		ep=epoll_create1(0);
		epp.events=EPOLLIN;
		epp.data.u32=1;
		epoll_ctl(ep,EPOLL_CTL_ADD,sockl,&epp);
		sockheart=sendtoTV(EPORT,1,NULL,hb);
		epp.data.u32=2;
		epoll_ctl(ep,EPOLL_CTL_ADD,sockheart,&epp);

		while((i=epoll_wait(ep,&event,1,tmout)) != -1)
		{
			if(i)
			{
				if(event.data.u32 == 1)
				{
					i=sizeof(client);
					sockcli=accept(sockl,(struct sockaddr*)&client,&i);
					while((i=recv(sockcli,data,sizeof(data),0)) > 0)
					{
						data[i]='\0';
						printf("%s",data);
						if((lpsid=strstr(data,"SID: uuid:")) && (lpurl=strstr(data,"NOTIFY /")))
						{
							lpsid+=10;
							lpurl+=8;
							lpsid[36]=lpurl[4]='\0';
							
							for(j=0;j < N(sch);j++)
								if(!strcmp(sch[j].url,lpurl))
								{
									if(strcmp(sch[j].sid,lpsid))
									{
										strcpy(sch[j].sid,lpsid);
										sch[j].tm=time(NULL);
										printf("\nNew SID : %s\n",sch[j].sid);
									}
									break;
								}
						}
						if(i > lend+16 && strstr(data+i-lend-16,end))
						{
							sprintf(data,"HTTP/1.1 200 OK\r\n");
							send(sockcli,data,strlen(data),0);
						}
					}
					close(sockcli);
				}
				else
				{
					while((i=recv(sockheart,data,sizeof(data),0)) > 0)
					{
						data[i]='\0';
						printf("%s",data);
					}
					epoll_ctl(ep,EPOLL_CTL_DEL,sockheart,NULL);
					close(sockheart);
					sockheart=sendtoTV(EPORT,1,NULL,hb);
					epp.data.u32=2;
					epoll_ctl(ep,EPOLL_CTL_ADD,sockheart,&epp);
				}
			}
			else
			{
				i=time(NULL);
				for(j=0;j < N(sch);j++)
					if(sch[j].tm && i-sch[j].tm >= TMOUT)
					{
						sprintf(data,"SUBSCRIBE /%.3s/event_%c HTTP/1.1\r\nHost: %s:%i\r\nSID: uuid:%s\r\n\r\n",sch[j].url,sch[j].url[3],tvaddr,tvport,sch[j].sid);
						printf("%s time overflow %i\n%s",sch[j].url,i-sch[j].tm,data);
						sch[j].tm=i;
						sendtoTV(tvport,0,NULL,data);
					}
			}
			
			i=0;
			for(j=0;j < N(sch);j++)
				if(sch[j].tm && (sch[j].tm < i || !i))
					i=sch[j].tm;
			if(i)
				if((tmout=(TMOUT+i-time(NULL))*1000) < 0)
					tmout=0;
					
			printf("Waiting %ims...\n",tmout);
		}
	}
	else
	{
		req(i,cmd[i].val ? argv+4 : NULL);
		if(!strcmp(argv[3],"key"))
		{
			for(i=0;i < N(keys);i++)
				if(!strcmp(keys[i],argv[4]))
					break;
			if(i == N(keys))
			{
				printf("Invalid key !\n%s",keys[0]);
				for(i=1;i < N(keys);i++)
					printf(",%s",keys[i]);
				printf("\n");
			}
		}
	}

	return 0;
}
