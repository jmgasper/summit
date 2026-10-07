#include "BluetoothATTClientHaiku.h"
#include "BluetoothGATTHaiku.h"
#include "BluetoothTestTransportHaiku.h"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <random>
using namespace WebCore::BluetoothHaiku;
struct Peer : ATTTransport {
    std::deque<std::vector<uint8_t>> replies;
    std::vector<std::vector<uint8_t>> sent;
    std::vector<uint8_t> value;
    std::vector<uint8_t> prepared;
    bool echoBad {false}, failPrepare {false}, cancelled {false}, serviceChanged {false}, privateCharacteristic {false};
    std::optional<UUID> custom;
    unsigned writes {0};
    ATTResult send(std::span<const uint8_t> packet) override {
        sent.emplace_back(packet.begin(),packet.end());
        auto handle=packet.size()>=3 ? packet[1] | unsigned(packet[2])<<8 : 0;
        auto error=[&](uint8_t code){replies.push_back({1,packet[0],uint8_t(handle),uint8_t(handle>>8),code});};
        switch(packet[0]) {
        case 0x10:
            if(packet[5]==1){error(0x0a);break;}
            if(handle==1 && custom){std::vector<uint8_t> reply{0x11,20,1,0,30,0};reply.insert(reply.end(),custom->bytes.rbegin(),custom->bytes.rend());replies.push_back(std::move(reply));}
            else if(handle==1 && serviceChanged)replies.push_back({0x11,6,1,0,10,0,0x0f,0x18,31,0,40,0,1,0x18});
            else if(handle==1)replies.push_back({0x11,6,1,0,10,0,0x0f,0x18,11,0,30,0,0x0a,0x18});
            else error(0x0a);
            break;
        case 0x08:
            if(packet[5]==0x03 && handle==1 && custom){std::vector<uint8_t> reply{0x09,21,2,0,0x12,3,0};reply.insert(reply.end(),custom->bytes.rbegin(),custom->bytes.rend());replies.push_back(std::move(reply));}
            else if(packet[5]==0x02 && handle==1 && custom)replies.push_back({0x09,6,2,0,11,0,30,0});
            else if(packet[5]==0x03 && handle==31 && serviceChanged)replies.push_back({0x09,7,32,0,0x20,33,0,5,0x2a});
            else if(packet[5]==0x03 && handle==1)replies.push_back({0x09,7,2,0,0x12,3,0,uint8_t(privateCharacteristic?0x25:0x19),0x2a,6,0,0x08,7,0,0x26,0x2a});
            else error(0x0a);
            break;
        case 0x04:
            if(handle==4 && custom){std::vector<uint8_t> reply{0x05,2,4,0};reply.insert(reply.end(),custom->bytes.rbegin(),custom->bytes.rend());replies.push_back(std::move(reply));}
            else if(handle==34 && serviceChanged)replies.push_back({0x05,1,34,0,2,0x29});
            else if(handle==4)replies.push_back({0x05,1,4,0,2,0x29,5,0,1,0x29});
            else error(0x0a);
            break;
        case 0x0a:case 0x0c: {
            if(handle==11 && custom){std::vector<uint8_t> reply{0x0b};reply.insert(reply.end(),custom->bytes.rbegin(),custom->bytes.rend());replies.push_back(std::move(reply));break;}
            unsigned offset=packet[0]==0x0c ? packet[3] | unsigned(packet[4])<<8 : 0;
            if(offset>value.size()){error(7);break;}
            std::vector<uint8_t> reply{uint8_t(packet[0]==0x0a?0x0b:0x0d)};
            reply.insert(reply.end(),value.begin()+offset,value.begin()+std::min<size_t>(offset+22,value.size()));replies.push_back(std::move(reply));break;
        }
        case 0x12:case 0x52:
            value.assign(packet.begin()+3,packet.end());writes++;if(packet[0]==0x12)replies.push_back({0x13});break;
        case 0x16: {
            if(failPrepare){error(0x09);break;}
            unsigned offset=packet[3] | unsigned(packet[4])<<8;
            if(offset!=prepared.size()){error(7);break;}
            prepared.insert(prepared.end(),packet.begin()+5,packet.end());
            std::vector<uint8_t> echo(packet.begin(),packet.end());echo[0]=0x17;if(echoBad)echo[1]^=1;replies.push_back(std::move(echo));break;
        }
        case 0x18:
            if(packet[1]){value=prepared;writes++;}else cancelled=true;prepared.clear();replies.push_back({0x19});break;
        case 0x1e:case 0x03:break;
        default:assert(false);
        }
        return {};
    }
    ATTResult receive(std::vector<uint8_t>& packet,unsigned) override {
        if(replies.empty())return {ATTError::Timeout};
        packet=std::move(replies.front());replies.pop_front();return {};
    }
};
int main()
{
    assert(UUID::alias(0x180f).string()=="0000180f-0000-1000-8000-00805f9b34fb");
    assert(UUID::alias(0x12345678).string()=="12345678-0000-1000-8000-00805f9b34fb");
    auto custom=*UUID::parse("12345678-90ab-cdef-1234-567890abcdef");
    std::vector<uint8_t> wire(custom.bytes.rbegin(),custom.bytes.rend());assert(UUID::fromWire(wire)==custom);
    assert(!namedUUID(UUIDKind::Service,"0000180F-0000-1000-8000-00805f9b34fb"));
    assert(namedUUID(UUIDKind::Service,"battery_service")==UUID::alias(0x180f));
    assert(namedUUID(UUIDKind::Characteristic,"battery_level")==UUID::alias(0x2a19));
    assert(namedUUID(UUIDKind::Descriptor,"gatt.client_characteristic_configuration")==UUID::alias(0x2902));
    assert(!namedUUID(UUIDKind::Service,"battery_level"));assert(!UUID::parse("bad-uuid"));
    assert(blocked(UUID::alias(0x1812)));assert(blocked(UUID::alias(0x2a25)));assert(!blocked(UUID::alias(0x2902),Access::Read));assert(blocked(UUID::alias(0x2902),Access::Write));
    Advertisement device;device.connectable=true;
    const std::vector<uint8_t> advertising{3,3,0x0f,0x18,5,9,'T','e','s','t',5,0xff,0x34,0x12,0xaa,0xbb};
    assert(device.append(advertising));Filter filter;filter.namePrefix="Te";filter.services={UUID::alias(0x180f)};filter.manufacturerData[0x1234]={{0xa0},{0xf0}};
    assert(filter.valid()&&filter.matches(device));Filter excluded;excluded.name="Test";assert(!matches(device,{filter},{excluded}));
    auto unchanged=device.name;assert(!device.append(std::vector<uint8_t>{5,9,'X'}));assert(device.name==unchanged);
    assert(!Filter{}.valid());Filter bad;bad.namePrefix="";assert(!bad.valid());
    Peer peer;ATTClient client(peer);std::vector<Service> services;assert(client.services(services));assert(services.size()==2&&services[0].uuid==UUID::alias(0x180f));
    std::vector<Characteristic> chars;assert(client.characteristics(services[0],chars));assert(chars.size()==2&&chars[0].value==3&&chars[0].end==5&&chars[1].end==10);
    std::vector<Descriptor> descriptors;assert(client.descriptors(chars[0],descriptors));assert(descriptors.size()==2&&descriptors[0].uuid==UUID::alias(0x2902));
    for(size_t length:{0,1,20,22,23,44,255,512}) {
        peer.value.resize(length);for(size_t i=0;i<length;i++)peer.value[i]=i;
        std::vector<uint8_t> read{0xff};assert(client.read(3,read));assert(read==peer.value);
        std::vector<uint8_t> write(length,0xaf);assert(client.write(3,write));assert(peer.value==write);
    }
    assert(client.write(3,std::vector<uint8_t>(513)).error==ATTError::InvalidLength);
    assert(client.write(3,std::vector<uint8_t>(21),false).error==ATTError::InvalidLength);
    assert(client.write(3,std::vector<uint8_t>(20),false));
    auto writes=peer.writes;peer.echoBad=true;assert(client.write(3,std::vector<uint8_t>(40)).error==ATTError::Protocol);assert(peer.cancelled&&peer.writes==writes);peer.echoBad=false;peer.cancelled=false;
    peer.failPrepare=true;assert(client.write(3,std::vector<uint8_t>(40)).remoteError==9);assert(peer.cancelled&&peer.writes==writes);peer.failPrepare=false;
    peer.replies.push_back({0x1d,3,0,88});std::vector<uint8_t> read;assert(client.read(3,read));
    std::vector<Notification> notifications;assert(client.pollNotifications(notifications));assert(notifications.size()==1&&notifications[0].handle==3&&notifications[0].value==std::vector<uint8_t>{88});
    assert(std::ranges::find(peer.sent,std::vector<uint8_t>{0x1e})!=peer.sent.end());
    peer.replies.push_back({0x09,7,0,0,2});assert(client.read(3,read).error==ATTError::Protocol);peer.replies.clear();
    Peer extended;extended.custom=custom;ATTClient extendedClient(extended);
    std::vector<Service> extendedServices,included;assert(extendedClient.services(extendedServices));assert(extendedServices.size()==1&&extendedServices[0].uuid==custom);
    assert(extendedClient.includedServices(extendedServices[0],included));assert(included.size()==1&&included[0].uuid==custom&&included[0].start==11);
    std::vector<Characteristic> extendedChars;assert(extendedClient.characteristics(extendedServices[0],extendedChars));assert(extendedChars.size()==1&&extendedChars[0].uuid==custom);
    std::vector<Descriptor> extendedDescriptors;assert(extendedClient.descriptors(extendedChars[0],extendedDescriptors));assert(extendedDescriptors.size()==1&&extendedDescriptors[0].uuid==custom);
    assert(device.append(std::vector<uint8_t>{4,0xff,0x4c,0,1}));assert(device.manufacturerData.contains(0x4c));
    assert(device.append(std::vector<uint8_t>{4,0xff,0x4c,0,2}));assert(!device.manufacturerData.contains(0x4c));
    Peer guardedPeer;ATTClient guardedClient(guardedPeer);
    GATTConnection gatt(guardedClient,{UUID::alias(0x180f),UUID::alias(0x1812)});
    assert(gatt.initialize());
    std::vector<Service> allowedServices;
    assert(gatt.services({},allowedServices)&&allowedServices.size()==1&&allowedServices[0].start==1);
    auto packets=guardedPeer.sent.size();
    assert(gatt.services(UUID::alias(0x180a),allowedServices).error==GATTError::Security);
    assert(gatt.services(UUID::alias(0x1812),allowedServices).error==GATTError::Security);
    std::vector<GATTCharacteristic> allowedChars;
    assert(gatt.characteristics(11,{},allowedChars).error==GATTError::Security);
    assert(gatt.characteristics(1,UUID::alias(0x2a25),allowedChars).error==GATTError::Security);
    assert(gatt.read(3,false,read).error==GATTError::Security);
    assert(guardedPeer.sent.size()==packets);
    assert(gatt.characteristics(1,{},allowedChars)&&allowedChars.size()==2);
    guardedPeer.value={88};assert(gatt.read(3,false,read)&&read==std::vector<uint8_t>{88});
    assert(gatt.write(3,false,std::vector<uint8_t>{99},true).error==GATTError::NotSupported);
    assert(gatt.write(7,false,std::vector<uint8_t>{99},true));
    assert(gatt.write(7,false,std::vector<uint8_t>{99},false).error==GATTError::NotSupported);
    std::vector<GATTDescriptor> allowedDescriptors;
    assert(gatt.descriptors(3,{},allowedDescriptors)&&allowedDescriptors.size()==2);
    packets=guardedPeer.sent.size();
    assert(gatt.write(4,true,std::vector<uint8_t>{1,0},true).error==GATTError::Security);
    assert(gatt.read(0xffff,true,read).error==GATTError::Security);
    assert(guardedPeer.sent.size()==packets);
    assert(gatt.notifications(3,true));assert(guardedPeer.sent.back()==std::vector<uint8_t>({0x12,4,0,1,0}));
    guardedPeer.replies.push_back({0x1b,3,0,77});guardedPeer.replies.push_back({0x1b,0xff,0xff,99});
    assert(gatt.poll(notifications)&&notifications.size()==1&&notifications[0].value==std::vector<uint8_t>{77});
    assert(gatt.notifications(3,false));
    guardedPeer.replies.push_back({0x1b,3,0,88});assert(gatt.poll(notifications)&&notifications.empty());
    gatt.invalidate();packets=guardedPeer.sent.size();assert(gatt.read(3,false,read).error==GATTError::InvalidState);
    assert(guardedPeer.sent.size()==packets);
    Peer changedPeer;changedPeer.serviceChanged=true;ATTClient changedClient(changedPeer);
    GATTConnection changing(changedClient,{UUID::alias(0x180f)});assert(changing.initialize());
    assert(std::ranges::find(changedPeer.sent,std::vector<uint8_t>{0x12,34,0,2,0})!=changedPeer.sent.end());
    assert(changing.services({},allowedServices)&&allowedServices.size()==1);
    assert(changing.characteristics(1,{},allowedChars));
    changedPeer.replies.push_back({0x1d,33,0,1,0,0xff,0xff});
    assert(changing.poll(notifications).error==GATTError::InvalidState&&notifications.empty());
    packets=changedPeer.sent.size();assert(changing.read(3,false,read).error==GATTError::InvalidState);
    assert(changedPeer.sent.size()==packets);
    Peer privatePeer;privatePeer.privateCharacteristic=true;ATTClient privateClient(privatePeer);
    GATTConnection privateGatt(privateClient,{UUID::alias(0x180f)});assert(privateGatt.initialize());
    assert(privateGatt.services({},allowedServices));assert(privateGatt.characteristics(1,{},allowedChars)&&allowedChars.size()==1);
    packets=privatePeer.sent.size();assert(privateGatt.read(3,false,read).error==GATTError::Security);assert(privatePeer.sent.size()==packets);
    TestTransport testPeer; ATTClient testClient(testPeer); GATTConnection testGatt(testClient,{UUID::alias(0x180f),UUID::alias(0x180a)});
    assert(testGatt.initialize());
    assert(testGatt.services({},allowedServices)&&allowedServices.size()==2);
    assert(testGatt.characteristics(1,{},allowedChars)&&allowedChars.size()==1);
    assert(testGatt.read(3,false,read)&&read==std::vector<uint8_t>{88});
    assert(testGatt.descriptors(3,{},allowedDescriptors)&&allowedDescriptors.size()==2);
    assert(testGatt.write(5,true,std::vector<uint8_t>{'O','K'},true));
    assert(testGatt.read(5,true,read)&&read==std::vector<uint8_t>({'O','K'}));
    std::vector<uint8_t> longValue(512,77);
    assert(testGatt.write(3,false,longValue,true)&&testGatt.read(3,false,read)&&read==longValue);
    assert(testGatt.includedServices(1,{},allowedServices)&&allowedServices.size()==1&&allowedServices[0].start==11);
    assert(testGatt.notifications(3,true));assert(testGatt.poll(notifications)&&notifications.size()==1);
    assert(testGatt.write(3,false,std::vector<uint8_t>{254},true));
    assert(testGatt.poll(notifications).error==GATTError::InvalidState);
    std::mt19937 random(0x626c65);
    for(unsigned i=0;i<5000;i++) {
        std::vector<uint8_t> fuzz(random()%64);for(auto& byte:fuzz)byte=random();Advertisement advertisement;advertisement.append(fuzz);
        Peer malformed;malformed.replies.push_back(fuzz);ATTClient test(malformed);std::vector<Service> output;test.services(output);
    }
    std::cout<<"BLUETOOTH_PORTABLE_PASS mutations=5000 aliases=268 long_read_write=512 capability_checks=pass\n";
}
