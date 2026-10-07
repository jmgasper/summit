'use strict';
window.btTest = (() => {
    let operation = null, device = null, results = [];
    const assert = (value, label) => { results.push({label, pass:!!value}); if (!value) throw Error(label); };
    const rejection = async (action, name, label) => {
        let actual = 'resolved'; try { await action(); } catch (e) { actual = e.name; }
        assert(actual === name, `${label}: expected ${name}, got ${actual}`);
    };
    const wait = async predicate => { const until=Date.now()+5000; while (!predicate()) { if(Date.now()>until) throw Error('event timeout'); await new Promise(r=>setTimeout(r,30)); } };
    const canonical = n => BluetoothUUID.canonicalUUID(n);
    const methods = {
        async capabilities() {
            results = [];
            assert(isSecureContext && !!navigator.bluetooth, 'secure context exposes navigator.bluetooth');
            assert(navigator.bluetooth === navigator.bluetooth, 'navigator Bluetooth is SameObject');
            for (const name of ['Bluetooth','BluetoothDevice','BluetoothRemoteGATTServer','BluetoothRemoteGATTService','BluetoothRemoteGATTCharacteristic','BluetoothRemoteGATTDescriptor','BluetoothCharacteristicProperties']) {
                assert(typeof window[name] === 'function', `${name} interface exists`);
                await rejection(()=>new window[name], 'TypeError', `${name} is not directly constructible`);
            }
            assert(BluetoothUUID.getService('battery_service') === canonical(0x180f), 'service name resolves');
            assert(BluetoothUUID.getCharacteristic('battery_level') === canonical(0x2a19), 'characteristic name resolves');
            assert(BluetoothUUID.getDescriptor('gatt.client_characteristic_configuration') === canonical(0x2902), 'descriptor name resolves');
            assert(canonical(0x12345678) === '12345678-0000-1000-8000-00805f9b34fb', '32-bit UUID expands');
            await rejection(()=>BluetoothUUID.getService('battery_level'), 'TypeError', 'UUID names are scoped by kind');
            await rejection(()=>BluetoothUUID.getService('0000180F-0000-1000-8000-00805f9b34fb'), 'TypeError', 'UUID strings must be canonical lowercase');
            await rejection(()=>BluetoothUUID.canonicalUUID(-1), 'TypeError', 'canonicalUUID enforces the unsigned range');
            await rejection(()=>BluetoothUUID.getService('battery_service\0suffix'), 'TypeError', 'UUID names cannot hide a suffix after NUL');
            await rejection(()=>BluetoothUUID.getService('not-a-uuid'), 'TypeError', 'invalid UUID rejects');
            const value = {meaning:42}; const event = new ValueEvent('availabilitychanged', {value});
            assert(event.value === value && event.value === event.value && !event.isTrusted, 'ValueEvent retains the JS value identity');
            assert(typeof await navigator.bluetooth.getAvailability() === 'boolean', 'availability resolves a boolean');
            assert((await navigator.bluetooth.getDevices()).length === 0, 'fresh context has no device grants');
            await rejection(()=>navigator.bluetooth.requestDevice({}), 'TypeError', 'empty request options reject');
            await rejection(()=>navigator.bluetooth.requestDevice({acceptAllDevices:true}), 'SecurityError', 'requestDevice requires real user activation');
            return results;
        },
        async validation() {
            results = [];
            for (const options of [
                {filters:[]}, {filters:[{}]}, {filters:[{services:[]}]}, {filters:[{namePrefix:''}]},
                {filters:[{name:'x'.repeat(249)}]}, {filters:[{services:['unknown-service']}]},
                {filters:[{manufacturerData:[]}]}, {filters:[{serviceData:[]}]},
                {filters:[{manufacturerData:[{companyIdentifier:1, mask:new Uint8Array(1)}]}]},
                {filters:[{manufacturerData:[{companyIdentifier:1, dataPrefix:new Uint8Array(2), mask:new Uint8Array(1)}]}]},
                {acceptAllDevices:true,filters:[{name:'x'}]},
                {acceptAllDevices:true,exclusionFilters:[{name:'x'}]},
                {filters:[{name:'x'}],exclusionFilters:[]}
            ]) await rejection(()=>navigator.bluetooth.requestDevice(options),'TypeError','invalid filter rejects');
            await rejection(()=>navigator.bluetooth.requestDevice({filters:[{services:['human_interface_device']}]}),'SecurityError','HID filter is blocked');
            await rejection(()=>navigator.bluetooth.requestDevice({filters:[{name:'Summit Test Peripheral\0suffix'}]}),'NotFoundError','name filters retain all bytes after NUL');
            return results;
        },
        async request(options) {
            device = await navigator.bluetooth.requestDevice(options);
            let battery=null;
            if (new URL(location.href).searchParams.has('browser')) {
                const server=await device.gatt.connect();
                battery=(await (await (await server.getPrimaryService('battery_service')).getCharacteristic('battery_level')).readValue()).getUint8(0);
            }
            return {id:device.id, name:device.name, connected:device.gatt.connected, battery};
        },
        async grantChecks() {
            results = [];
            const listed = await navigator.bluetooth.getDevices();
            assert(listed.length === 1 && listed[0] === device, 'getDevices returns the granted device with stable identity');
            assert(device.gatt === device.gatt && device.gatt.device === device, 'GATT server and device identity');
            assert(!device.gatt.connected, 'a chooser grant does not implicitly connect');
            await rejection(()=>device.gatt.getPrimaryService('battery_service'),'NetworkError','disconnected discovery rejects');
            return results;
        },
        async gattChecks() {
            results = [];
            const server = await device.gatt.connect();
            assert(server === device.gatt && server.connected, 'connect resolves the live server');
            assert(await server.connect() === server, 'connecting an already connected server is stable');
            const services = await server.getPrimaryServices();
            assert(services.length === 2, 'only granted primary services are exposed');
            const service = await server.getPrimaryService('battery_service');
            assert(service === await server.getPrimaryService(0x180f) && service.device === device && service.isPrimary, 'service cache and parent identity');
            await rejection(()=>server.getPrimaryService('human_interface_device'),'SecurityError','blocked HID service remains inaccessible');
            await rejection(()=>server.getPrimaryService('heart_rate'),'SecurityError','ungranted service remains inaccessible');
            const included = await service.getIncludedServices();
            assert(included.length === 1 && included[0].uuid === canonical(0x180a), 'included service discovery respects grants');
            const characteristics = await service.getCharacteristics();
            assert(characteristics.length === 1, 'blocklisted serial characteristic is omitted');
            const characteristic = await service.getCharacteristic('battery_level');
            assert(characteristic === characteristics[0] && characteristic.service === service, 'characteristic cache and parent identity');
            const properties = characteristic.properties;
            assert(properties === characteristic.properties && properties.read && properties.write && properties.writeWithoutResponse && properties.notify && !properties.indicate, 'characteristic properties match ATT flags');
            await rejection(()=>service.getCharacteristic('serial_number_string'),'SecurityError','blocked serial read discovery rejects');
            assert(characteristic.value === null, 'unread value starts null');
            let readEvent = null; const once = () => readEvent = characteristic.value;
            characteristic.addEventListener('characteristicvaluechanged', once, {once:true});
            const first = await characteristic.readValue();
            assert(first instanceof DataView && first === characteristic.value && first === readEvent && first.getUint8(0) === 88, 'read resolves the cached DataView seen by the event');
            const data = new Uint8Array(512); for(let i=0;i<data.length;i++) data[i]=i%251;
            const pending = characteristic.writeValueWithResponse(data); data.fill(99); await pending;
            assert(characteristic.value.byteLength === 512 && characteristic.value.getUint8(1) === 1, 'long write snapshots input and updates cached value');
            const long = await characteristic.readValue();
            assert(long.byteLength === 512 && long.getUint8(510) === 8, '512-byte Read Blob result');
            await rejection(()=>characteristic.writeValueWithResponse(new Uint8Array(513)),'InvalidModificationError','oversized write rejects');
            await rejection(()=>characteristic.writeValueWithoutResponse(new Uint8Array(21)),'InvalidModificationError','write command respects MTU');
            await characteristic.writeValueWithoutResponse(new Uint8Array([70]));
            assert((await characteristic.readValue()).getUint8(0) === 70, 'write without response reaches the peripheral');
            const descriptors = await characteristic.getDescriptors();
            assert(descriptors.length === 2, 'descriptor discovery');
            const description = await characteristic.getDescriptor('gatt.characteristic_user_description');
            assert(description.characteristic === characteristic && description === await characteristic.getDescriptor(0x2901), 'descriptor cache and parent identity');
            const text = await description.readValue(); assert(text === description.value && text.byteLength === 7, 'descriptor reads preserve DataView identity');
            await description.writeValue(new Uint8Array([79,75]));
            assert(description.value.getUint8(0) === 79 && (await description.readValue()).byteLength === 2, 'descriptor write updates value and peripheral');
            const cccd = await characteristic.getDescriptor(0x2902);
            await rejection(()=>cccd.writeValue(new Uint8Array([1,0])),'SecurityError','direct notification descriptor write is blocked');
            const events = []; const parents = [];
            const listener = event => { events.push(characteristic.value.getUint8(0)); assert(event.target === characteristic,'notification target'); };
            const parent = event => parents.push(event.currentTarget);
            characteristic.addEventListener('characteristicvaluechanged',listener);
            for(const object of [service, device, navigator.bluetooth]) object.addEventListener('characteristicvaluechanged',parent);
            assert(await characteristic.startNotifications() === characteristic,'notifications resolve the characteristic');
            await wait(()=>events.includes(70));
            await characteristic.writeValueWithResponse(new Uint8Array([71])); await wait(()=>events.includes(71));
            assert(parents.includes(service) && parents.includes(device) && parents.includes(navigator.bluetooth),'notification bubbles through service, device and Bluetooth');
            assert(await characteristic.stopNotifications() === characteristic,'stop notifications resolves the characteristic');
            const count = events.length; await characteristic.writeValueWithResponse(new Uint8Array([72])); await new Promise(r=>setTimeout(r,300));
            assert(events.length === count,'stopped notifications do not fire');
            characteristic.removeEventListener('characteristicvaluechanged',listener);
            for(const object of [service, device, navigator.bluetooth]) object.removeEventListener('characteristicvaluechanged',parent);
            let disconnected=0; device.addEventListener('gattserverdisconnected',()=>++disconnected);
            device.gatt.disconnect(); assert(!device.gatt.connected && disconnected === 1,'explicit disconnect updates state and emits once');
            device.gatt.disconnect(); assert(disconnected === 1,'repeated disconnect is idempotent');
            await device.gatt.connect();
            await rejection(()=>characteristic.readValue(),'InvalidStateError','old characteristic cannot cross connection generations');
            const fresh = await (await device.gatt.getPrimaryService(0x180f)).getCharacteristic(0x2a19);
            assert(fresh !== characteristic,'reconnect creates fresh GATT objects');
            await fresh.writeValueWithResponse(new Uint8Array([254]));
            await wait(()=>!device.gatt.connected);
            assert(disconnected === 2,'Service Changed invalidates handles and disconnects');
            return results;
        },
        async busy() { results=[]; await rejection(()=>device.gatt.connect(),'InvalidStateError','busy device rejects without taking the link'); assert(!device.gatt.connected,'busy server remains disconnected'); return results; },
        async forget() { const previous=device; await previous.forget(); await previous.forget(); const devices=await navigator.bluetooth.getDevices(); return {count:devices.length, connected:previous.gatt.connected}; },
        async devices() { return (await navigator.bluetooth.getDevices()).map(d=>({id:d.id,name:d.name})); },
        async probe() { let devices=null, error=null; try { devices=(await navigator.bluetooth.getDevices()).length; } catch(e) { error=e.name; } return {secure:isSecureContext, api:typeof navigator.bluetooth, available:navigator.bluetooth?await navigator.bluetooth.getAvailability():null, devices,error}; },
        async frame(cross, allow) {
            const frame=document.createElement('iframe'); if(allow!==null) frame.allow=allow;
            const url=new URL(location.href); if(cross) url.hostname=location.hostname==='localhost'?'127.0.0.1':'localhost'; url.search='?probe=1';
            const result=new Promise((resolve,reject)=>{ const timeout=setTimeout(()=>reject(Error('frame timeout')),5000); const listener=e=>{if(e.source!==frame.contentWindow)return;clearTimeout(timeout);removeEventListener('message',listener);resolve(e.data);};addEventListener('message',listener); });
            frame.src=url; document.body.append(frame); try{return await result;}finally{frame.remove();}
        }
    };
    const run=(method,...args)=>{operation=null;Promise.resolve().then(()=>methods[method](...args)).then(value=>{operation={ok:true,value};resultsElement.textContent=JSON.stringify(operation,null,2);report();},e=>{operation={ok:false,error:e.name,message:e.message,results};resultsElement.textContent=JSON.stringify(operation,null,2);report();});return true;};
    const resultsElement=document.getElementById('results');
    const report=()=>{if(!new URL(location.href).searchParams.has('browser'))return;const r=document.getElementById('request').getBoundingClientRect();document.title='BLUETOOTH '+JSON.stringify({ready:true,button:[r.x+r.width/2,r.y+r.height/2],operation});};
    requestAnimationFrame(report);
    document.getElementById('request').onclick=()=>run(window.nextMethod||'request', window.nextOptions || {filters:[{name:'Summit Test Peripheral'}],optionalServices:['battery_service','device_information','human_interface_device']});
    if(new URL(location.href).searchParams.has('probe')) methods.probe().then(value=>parent.postMessage(value,'*'));
    return {run,operation:()=>operation};
})();
