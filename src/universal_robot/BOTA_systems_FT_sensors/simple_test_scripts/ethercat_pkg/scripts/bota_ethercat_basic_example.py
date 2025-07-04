"""Prints the readings of a Bota Systems EtherCAT sensor.

Usage: python bota_basic_example.py <adapter>

This example expects a physical slave layout according to
_expected_slave_layout, see below.
"""

import sys
import struct
import ctypes
import time
import threading

from collections import namedtuple

import pysoem


class BasicExample:

    BOTA_VENDOR_ID = 0xB07A
    BOTA_PRODUCT_CODE = 0x00000001
    SINC_LENGTH = 256
    # Note that the time step is set according to the sinc filter size!
    time_step = 1.0;

    def __init__(self, ifname):
        self._ifname = ifname
        self._pd_thread_stop_event = threading.Event()
        self._ch_thread_stop_event = threading.Event()
        self._actual_wkc = 0
        self._master = pysoem.Master()
        self._master.in_op = False
        self._master.do_check_state = False
        SlaveSet = namedtuple('SlaveSet', 'name product_code config_func')
        self._expected_slave_layout = {0: SlaveSet('BFT-MEDS-ECAT-M8', self.BOTA_PRODUCT_CODE, self.bota_sensor_setup)}

    def bota_sensor_setup(self, slave_pos):
        print("bota_sensor_setup")
        slave = self._master.slaves[slave_pos]

        ## Set sensor configuration
        # calibration matrix active
        slave.sdo_write(0x8010, 1, bytes(ctypes.c_uint8(1)))
        # temperature compensation
        slave.sdo_write(0x8010, 2, bytes(ctypes.c_uint8(0)))
        # IMU active
        slave.sdo_write(0x8010, 3, bytes(ctypes.c_uint8(1)))

        ## Set force torque filter
        # FIR disable
        slave.sdo_write(0x8006, 2, bytes(ctypes.c_uint8(1)))
        # FAST enable
        slave.sdo_write(0x8006, 3, bytes(ctypes.c_uint8(0)))
        # CHOP enable
        slave.sdo_write(0x8006, 4, bytes(ctypes.c_uint8(0)))
        # Sinc filter size
        slave.sdo_write(0x8006, 1, bytes(ctypes.c_uint16(self.SINC_LENGTH)))

        ## Get sampling rate
        sampling_rate = struct.unpack('h', slave.sdo_read(0x8011, 0))[0]
        print("Sampling rate {}".format(sampling_rate))
        if sampling_rate > 0:
            self.time_step = 1.0/float(sampling_rate)

        print("time step {}".format(self.time_step))

    def _processdata_thread(self):
        while not self._pd_thread_stop_event.is_set():
            start_time = time.perf_counter()
            self._master.send_processdata()
            self._actual_wkc = self._master.receive_processdata(2000)
            if not self._actual_wkc == self._master.expected_wkc:
                print('incorrect wkc')

            sensor_input_as_bytes = self._master.slaves[0].input
            status = struct.unpack_from('B', sensor_input_as_bytes, 0)[0]

            warningsErrorsFatals = struct.unpack_from('I', sensor_input_as_bytes, 1)[0]

            Fx = struct.unpack_from('f', sensor_input_as_bytes, 5)[0]
            Fy = struct.unpack_from('f', sensor_input_as_bytes, 9)[0]
            Fz = struct.unpack_from('f', sensor_input_as_bytes, 13)[0]
            Mx = struct.unpack_from('f', sensor_input_as_bytes, 17)[0]
            My = struct.unpack_from('f', sensor_input_as_bytes, 21)[0]
            Mz = struct.unpack_from('f', sensor_input_as_bytes, 25)[0]
            forceTorqueSaturated = struct.unpack_from('H', sensor_input_as_bytes, 29)[0]

            Ax = struct.unpack_from('f', sensor_input_as_bytes, 31)[0]
            Ay = struct.unpack_from('f', sensor_input_as_bytes, 35)[0]
            Az = struct.unpack_from('f', sensor_input_as_bytes, 39)[0]
            accelerationSaturated = struct.unpack_from('B', sensor_input_as_bytes, 43)[0]

            Rx = struct.unpack_from('f', sensor_input_as_bytes, 44)[0]
            Ry = struct.unpack_from('f', sensor_input_as_bytes, 48)[0]
            Rz = struct.unpack_from('f', sensor_input_as_bytes, 52)[0]
            angularRateSaturated = struct.unpack_from('B', sensor_input_as_bytes, 56)[0]

            temperature = struct.unpack_from('f', sensor_input_as_bytes, 57)[0]
            
            #print("Status {}".format(status))
            #print("Warnings/Errors/Fatals {}".format(warningsErrorsFatals))

            print("Fx {}".format(Fx))
            print("Fy {}".format(Fy))
            print("Fz {}".format(Fz))
            print("Mx {}".format(Mx))
            print("My {}".format(My))
            print("Mz {}".format(Mz))
            #print("Force-Torque saturated {}".format(forceTorqueSaturated))
            
            # print("Ax {}".format(Ax))
            # print("Ay {}".format(Ay))
            # print("Az {}".format(Az))
            #print("Acceleration saturated {}".format(accelerationSaturated))
            
            #print("Rx {}".format(Rx))
            #print("Ry {}".format(Ry))
            #print("Rz {}".format(Rz))
            #print("Angular rate saturated {}".format(angularRateSaturated))
            #print("Temperature {}\n".format(temperature))
                
            time_diff = time.perf_counter() - start_time
            if time_diff < self.time_step:
                BasicExample._sleep(self.time_step - time_diff)
 

    def _my_loop(self):

        self._master.in_op = True

        try:
            while 1:
                print('Run my loop')

                time.sleep(1.0)

        except KeyboardInterrupt:
            # ctrl-C abort handling
            print('stopped')


    def run(self):

        self._master.open(self._ifname)

        if not self._master.config_init() > 0:
            self._master.close()
            raise BasicExampleError('no slave found')

        for i, slave in enumerate(self._master.slaves):
            if not ((slave.man == self.BOTA_VENDOR_ID) and
                    (slave.id == self._expected_slave_layout[i].product_code)):
                self._master.close()
                raise BasicExampleError('unexpected slave layout')
            slave.config_func = self._expected_slave_layout[i].config_func
            slave.is_lost = False

        self._master.config_map()

        if self._master.state_check(pysoem.SAFEOP_STATE, 50000) != pysoem.SAFEOP_STATE:
            self._master.close()
            raise BasicExampleError('not all slaves reached SAFEOP state')

        self._master.state = pysoem.OP_STATE

        check_thread = threading.Thread(target=self._check_thread)
        check_thread.start()
        proc_thread = threading.Thread(target=self._processdata_thread)
        proc_thread.start()
        
        # send one valid process data to make outputs in slaves happy
        self._master.send_processdata()
        self._master.receive_processdata(2000)
        # request OP state for all slaves
        
        self._master.write_state()

        all_slaves_reached_op_state = False
        for i in range(40):
            self._master.state_check(pysoem.OP_STATE, 50000)
            if self._master.state == pysoem.OP_STATE:
                all_slaves_reached_op_state = True
                break

        if all_slaves_reached_op_state:
            self._my_loop()

        self._pd_thread_stop_event.set()
        self._ch_thread_stop_event.set()
        proc_thread.join()
        check_thread.join()
        self._master.state = pysoem.INIT_STATE
        # request INIT state for all slaves
        self._master.write_state()
        self._master.close()

        if not all_slaves_reached_op_state:
            raise BasicExampleError('not all slaves reached OP state')

    @staticmethod
    def _check_slave(slave, pos):
        if slave.state == (pysoem.SAFEOP_STATE + pysoem.STATE_ERROR):
            print(
                'ERROR : slave {} is in SAFE_OP + ERROR, attempting ack.'.format(pos))
            slave.state = pysoem.SAFEOP_STATE + pysoem.STATE_ACK
            slave.write_state()
        elif slave.state == pysoem.SAFEOP_STATE:
            print(
                'WARNING : slave {} is in SAFE_OP, try change to OPERATIONAL.'.format(pos))
            slave.state = pysoem.OP_STATE
            slave.write_state()
        elif slave.state > pysoem.NONE_STATE:
            if slave.reconfig():
                slave.is_lost = False
                print('MESSAGE : slave {} reconfigured'.format(pos))
        elif not slave.is_lost:
            slave.state_check(pysoem.OP_STATE)
            if slave.state == pysoem.NONE_STATE:
                slave.is_lost = True
                print('ERROR : slave {} lost'.format(pos))
        if slave.is_lost:
            if slave.state == pysoem.NONE_STATE:
                if slave.recover():
                    slave.is_lost = False
                    print(
                        'MESSAGE : slave {} recovered'.format(pos))
            else:
                slave.is_lost = False
                print('MESSAGE : slave {} found'.format(pos))
    
    def _check_thread(self):

        while not self._ch_thread_stop_event.is_set():
            if self._master.in_op and ((self._actual_wkc < self._master.expected_wkc) or self._master.do_check_state):
                self._master.do_check_state = False
                self._master.read_state()
                for i, slave in enumerate(self._master.slaves):
                    if slave.state != pysoem.OP_STATE:
                        self._master.do_check_state = True
                        BasicExample._check_slave(slave, i)
                if not self._master.do_check_state:
                    print('OK : all slaves resumed OPERATIONAL.')
            time.sleep(self.time_step)

    @staticmethod
    def _sleep(duration, get_now=time.perf_counter):
        now = get_now()
        end = now + duration
        while now < end:
            now = get_now()

class BasicExampleError(Exception):
    def __init__(self, message):
        super(BasicExampleError, self).__init__(message)
        self.message = message


if __name__ == '__main__':

    print('basic_example started')

    if len(sys.argv) > 1:
        try:
            BasicExample(sys.argv[1]).run()
        except BasicExampleError as expt:
            print('basic_example failed: ' + expt.message)
            sys.exit(1)
    else:
        print('usage: basic_example ifname')
        sys.exit(1)
