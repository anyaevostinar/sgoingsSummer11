#
# create.py
#
# Python interface for the iRobot Create
#
# Zach Dodds   dodds@cs.hmc.edu
# updated for SIGCSE 3/9/07

#create.py
#Written 2008-6-3 by Peter Mawhorter
#Based on pyCreate's create module... this is a minimal version of that code.

#create2.py
#Added code for wait for angle, wait for distance, wait for event (C.A. Berry) - 8/22/08
#Added code for sensor streaming, removed obsolete, unnecessary code (integrate odometery) (CAB) - 8/26/08

#create3.py
#Added the senseAndRetry code from prior versions
#Modified the moveTo(0 adn turnTo() functions to work with go instead of drive function

#NOTE: renamed back to create.py - 9/16/08

# v1.1 Added access to overcurrent; made sensor streaming optional in the Create constructor. This allows
# manual polling if desired. (M Boutell, 10/2/2008)

# v1.2 Added IR broadcast streaming functions, so a robot can send an IR signal 
# over the omnidirectional IR LEDs. Also Create constructor now calls setLED, 
# so the Create displays an amber light when the power is on.

# v2.0 Complete overhaul, removing many duplicate functions not used by students.
# Removed threaded sensor polling: less efficient if many sensors needed, but more predictable
# Added IR broadcast streaming using the scripting functionality of the OI, 
# so it can stream while moving without needing an additional thread.

# v2.1 Added constants for array indices when sensors return an array 

# v2.2 Added support for a simulator to connect via a network socket (RHIT Senior Project Team 9/12/09)

# v2.3 Added ability to retry getting sensor data.

# v3.0 (TODO: rename shutdown as disconnect)

#added by alexandra
from __future__ import division
from __future__ import print_function

version = 2.3


import serial
import socket
import math
import time
import select
#changed by alexandra
import thread # thread libs needed to lock serial port during transmissions
from threading import *


# The Create's baudrate and timeout:
baudrate = 57600
timeout = 0.5

# some module-level definitions for the robot commands
START = chr(128)    # already converted to bytes...
BAUD = chr(129)     # + 1 byte
CONTROL = chr(130)  # deprecated for Create
SAFE = chr(131)
FULL = chr(132)
POWER = chr(133)
SPOT = chr(134)     # Same for the Roomba and Create
CLEAN = chr(135)    # Clean button - Roomba
COVER = chr(135)    # Cover demo - Create
MAX = chr(136)      # Roomba
DEMO = chr(136)     # Create
DRIVE = chr(137)    # + 4 bytes
LEDS = chr(139)     # + 3 bytes
SONG = chr(140)     # + 2N+2 bytes, where N is the number of notes
PLAY = chr(141)     # + 1 byte
SENSORS = chr(142)  # + 1 byte
FORCESEEKINGDOCK = chr(143)  # same on Roomba and Create
# the above command is called "Cover and Dock" on the Create
DRIVEDIRECT = chr(145)       # Create only
STREAM = chr(148)       # Create only
QUERYLIST = chr(149)       # Create only
PAUSERESUME = chr(150)       # Create only
WAITTIME = chr(155)#Added by CAB, time in 1 data byte 1 in 10ths of a second
WAITDIST = chr(156)#Added by CAB, distance in 16-bit signed in mm
WAITANGLE = chr(157)#Added by CAB, angle in 16-bit signed in degrees 
WAITEVENT = chr(158)#Added by CAB, event in signed event number
# MB added these for scripting
DEFINE_SCRIPT = chr(152) 
RUN_SCRIPT = chr(153)

# the four SCI modes
# the code will try to keep track of which mode the system is in,
# but this might not be 100% trivial...
OFF_MODE = 0
PASSIVE_MODE = 1
SAFE_MODE = 2
FULL_MODE = 3

# Command codes are opcodes sent to the Create via serial. They define the
# possible message types.
COMMANDS = {
"START":chr(128),
"BAUD":chr(129),
"MODE_PASSIVE":chr(128),
"MODE_SAFE":chr(131),
"MODE_FULL":chr(132),
"DEMO":chr(136),
"DEMO_COVER":chr(135),
"DEMO_COVER_AND_DOCK":chr(143),
"DEMO_SPOT":chr(134),
"DRIVE":chr(137),
"DRIVE_DIRECT":chr(145),
"LEDS":chr(139),
"SONG":chr(140),
"PLAY_SONG":chr(141),
"SENSORS":chr(142),
"QUERY_LIST":chr(149),
"STREAM":chr(148),
"PAUSE/RESUME_STREAM":chr(150),
"DIGITAL_OUTPUTS":chr(147),
"LOW_SIDE_DRIVERS":chr(138),
"PWM_LOW_SIDE_DRIVERS":chr(144),
"SEND_IR":chr(151),
# MB added these for scripting
"DEFINE_SCRIPT":chr(152),
"RUN_SCRIPT":chr(153)
}
#TODO: define the rest of the command codes in the SCI.

# Constants for array indices when sensors return an array 
# Bumps and wheeldrops
WHEELDROP_CASTER = 0
WHEELDROP_LEFT = 1
WHEELDROP_RIGHT = 2
BUMP_LEFT = 3
BUMP_RIGHT = 4

# Buttons
BUTTON_ADVANCE = 0
BUTTON_PLAY = 1

# Overcurrents
LEFT_WHEEL = 0
RIGHT_WHEEL = 1
LD_2 = 2
LD_0 = 3
LD_1 = 4

# Use digital inputs
BAUD_RATE_CHANGE = 0
DIGITAL_INPUT_3 = 1
DIGITAL_INPUT_2 = 2
DIGITAL_INPUT_1 = 3
DIGITAL_INPUT_0 = 4

# Charging sources available
HOME_BASE = 0
INTERNAL_CHARGER = 1

# For the getSensor retry loop.
MIN_SENSOR_RETRIES = 2 # 1 s
RETRY_SLEEP_TIME = 0.5 # 50ms

class SensorModule:
        def __init__(self, packetID, parseMode, packetSize):
                self.ID =packetID
                self.interpret = parseMode
                self.size = packetSize

# Sensor codes are used to ask for data along with a QUERY command.
SENSORS = {
"BUMPS_AND_WHEEL_DROPS":SensorModule(chr(7),"ONE_BYTE_UNPACK",1),
"WALL":SensorModule(chr(8),"ONE_BYTE_UNSIGNED",1),
"CLIFF_LEFT":SensorModule(chr(9),"ONE_BYTE_UNSIGNED",1),
"CLIFF_FRONT_LEFT":SensorModule(chr(10),"ONE_BYTE_UNSIGNED",1),
"CLIFF_FRONT_RIGHT":SensorModule(chr(11),"ONE_BYTE_UNSIGNED",1),
"CLIFF_RIGHT":SensorModule(chr(12),"ONE_BYTE_UNSIGNED",1),
"VIRTUAL_WALL":SensorModule(chr(13),"ONE_BYTE_UNSIGNED",1),
"OVERCURRENTS":SensorModule(chr(14),"ONE_BYTE_UNPACK",1),
"IR_BYTE":SensorModule(chr(17),"ONE_BYTE_UNSIGNED",1),
"BUTTONS":SensorModule(chr(18),"ONE_BYTE_UNPACK",1),
"DISTANCE":SensorModule(chr(19),"TWO_BYTE_SIGNED",2),
"ANGLE":SensorModule(chr(20),"TWO_BYTE_SIGNED",2),
"CHARGING_STATE":SensorModule(chr(21),"ONE_BYTE_UNSIGNED",1),
"VOLTAGE":SensorModule(chr(22),"TWO_BYTE_UNSIGNED",2),
"CURRENT":SensorModule(chr(23),"TWO_BYTE_SIGNED",2),
"BATTERY_TEMPERATURE":SensorModule(chr(24),"ONE_BYTE_SIGNED",1),
"BATTERY_CHARGE":SensorModule(chr(25),"TWO_BYTE_UNSIGNED",2),
"BATTERY_CAPACITY":SensorModule(chr(26),"TWO_BYTE_UNSIGNED",2),
"WALL_SIGNAL":SensorModule(chr(27),"TWO_BYTE_UNSIGNED",2),
"CLIFF_LEFT_SIGNAL":SensorModule(chr(28),"TWO_BYTE_UNSIGNED",2),
"CLIFF_FRONT_LEFT_SIGNAL":SensorModule(chr(29),"TWO_BYTE_UNSIGNED",2),
"CLIFF_FRONT_RIGHT_SIGNAL":SensorModule(chr(30),"TWO_BYTE_UNSIGNED",2),
"CLIFF_RIGHT_SIGNAL":SensorModule(chr(31),"TWO_BYTE_UNSIGNED",2),
"USER_DIGITAL_INPUTS":SensorModule(chr(32),"ONE_BYTE_UNPACK",1),
"USER_ANALOG_INPUT":SensorModule(chr(33),"TWO_BYTE_UNSIGNED",2),
"CHARGING_SOURCES_AVAILABLE":SensorModule(chr(34),"ONE_BYTE_UNSIGNED",1),
"OI_MODE":SensorModule(chr(35),"ONE_BYTE_UNSIGNED",1),
"SONG_NUMBER":SensorModule(chr(36),"ONE_BYTE_UNSIGNED",1),
"SONG_PLAYING":SensorModule(chr(37),"ONE_BYTE_UNSIGNED",1),
"NUMBER_OF_STREAM_PACKETS":SensorModule(chr(38),"ONE_BYTE_UNSIGNED",1),
"VELOCITY":SensorModule(chr(39),"TWO_BYTE_SIGNED",2),
"RADIUS":SensorModule(chr(40),"TWO_BYTE_SIGNED",2),
"RIGHT_VELOCITY":SensorModule(chr(41),"TWO_BYTE_SIGNED",2),
"LEFT_VELOCITY":SensorModule(chr(42),"TWO_BYTE_SIGNED",2)
}

# Interpretation codes are used to tell how to deal with the raw data from a sensor query
# Note a negative value implies one byte of data is being dealt with (also includes 0), a positive implies 2 bytes
INTERPRET = {
"ONE_BYTE_UNPACK":-1,
"ONE_BYTE_SIGNED":-2,
"ONE_BYTE_UNSIGNED":-3,
"NO_HANDLING":0,
"TWO_BYTE_SIGNED":1,
"TWO_BYTE_UNSIGNED":2
}

# some module-level functions for dealing with bits and bytes
#
def bytesOfR( r ):
        """ for looking at the raw bytes of a sensor reply, r """
        print('raw r is', r)
        for i in range(len(r)):
            print('byte', i, 'is', ord(r[i]))
        print('finished with formatR')

def bitOfByte( bit, byte ):
        """ returns a 0 or 1: the value of the 'bit' of 'byte' """
        if bit < 0 or bit > 7:
            print('Your bit of', bit, 'is out of range (0-7)')
            print('returning 0')
            return 0
        return ((byte >> bit) & 0x01)

def toBinary( val, numBits ):
        """ prints numBits digits of val in binary """
        if numBits == 0:  return
        toBinary( val>>1 , numBits-1 )
        print((val & 0x01), end=' ')  # print least significant bit

def fromBinary( s ):
        """ s is a string of 0's and 1's """
        if s == '': return 0
        lowbit = ord(s[-1]) - ord('0')
        return lowbit + 2*fromBinary( s[:-1] )

def twosComplementInt1byte( byte ):
        """ returns an int of the same value of the input
        int (a byte), but interpreted in two's
        complement
        the output range should be -128 to 127
        """
        # take everything except the top bit
        topbit = bitOfByte( 7, byte )
        lowerbits = byte & 127
        if topbit == 1:
            return lowerbits - (1 << 7)
        else:
            return lowerbits

def twosComplementInt2bytes( highByte, lowByte ):
        """ returns an int which has the same value
        as the twosComplement value stored in
        the two bytes passed in
     
        the output range should be -32768 to 32767
     
        chars or ints can be input, both will be
        truncated to 8 bits
        """
        # take everything except the top bit
        topbit = bitOfByte( 7, highByte )
        lowerbits = highByte & 127
        unsignedInt = lowerbits << 8 | (lowByte & 0xFF)
        if topbit == 1:
            # with sufficient thought, I've convinced
            # myself of this... we'll see, I suppose.
            return unsignedInt - (1 << 15)
        else:
            return unsignedInt

def toTwosComplement2Bytes( value ):
        """ returns two bytes (ints) in high, low order
        whose bits form the input value when interpreted in
        two's complement
        """
        # if positive or zero, it's OK
        if value >= 0:
            eqBitVal = value
            # if it's negative, I think it is this
        else:
            eqBitVal = (1<<16) + value
    
        return ( (eqBitVal >> 8) & 0xFF, eqBitVal & 0xFF )

def displayVersion():
    print("pycreate version", version)


class CommunicationError(Exception):
  '''
  This error indicates that there was a problem communicating with the
  Create. The string msg indicates what went wrong.
  '''
  def __init__(self, msg):
    self.msg = msg
  def __str__(self):
    return str(self.msg)
  def __repr__(self):
    return "CommunicationError(" + repr(self.msg) + ")"
   

# ======================The CREATE ROBOT CLASS (modified by CAB 8/08)==========================
class Create:
    """ the Create class is an abstraction of the iRobot Create's
        SCI interface, including communication and a bit
        of processing of the strings passed back and forth
        
        when you create an object of type Create, the code
        will try to open a connection to it - so, it will fail
        if it's not attached!
    """
        
    # TODO: check if we can start in other modes...
#======================== Starting up and Shutting Down================    
    def __init__(self, PORT, startingMode=SAFE_MODE, sim_mode = False):
        """ the constructor which tries to open the
            connection to the robot at port PORT
        """
        # to do: find the shortest safe serial timeout value...
        # to do: use the timeout to do more error checking than
        #        is currently done...
        #
        # the -1 here is because windows starts counting from 1
        # in the hardware control panel, but not in pyserial, it seems
        
        displayVersion()
        
        # fields for simulator
        self.in_sim_mode = False
        self.sim_sock = None
        self.sim_host = '127.0.0.1'
        self.sim_port = 65000
        self.maxSensorRetries = MIN_SENSOR_RETRIES 
        
        # if PORT is the string 'simulated' (or any string for the moment)
        # we use our SRSerial class
        self.comPort = PORT   #we want to keep track of the port number for reconnect() calls
        print('PORT is', PORT)
        if type(PORT) == type('string'):
            if PORT == 'sim':
                self.init_sim_mode()
                self.ser = None
            else:
                # for Mac/Linux - use whole port name
                # print 'In Mac/Linux mode...'
                self.ser = serial.Serial(PORT, baudrate=57600, timeout=0.5)
        # otherwise, we try to open the numeric serial port...
                if (sim_mode):
                    self.init_sim_mode()
        else:
            # print 'In Windows mode...'
            try:
                self.ser = serial.Serial(PORT-1, baudrate=57600, timeout=0.5)
                if (sim_mode):
                    self.init_sim_mode()
            except serial.SerialException:
                print("unable to access the serial port - please cycle the robot's power")

        # did the serial port actually open?
        if self.in_sim_mode:
            print("In simulator mode")
        elif self.ser.isOpen():
            print('Serial port did open on iRobot Create...')
        else:
            print('Serial port did NOT open, check the')
            print('  - port number')
            print('  - physical connection')
            print('  - baud rate of the roomba (it\'s _possible_, if unlikely,')
            print('              that it might be set to 19200 instead')
            print('              of the default 57600 - removing and')
            print('              reinstalling the battery should reset it.')
        
        # define the class' Open Interface mode
        self.sciMode = OFF_MODE

        if (startingMode == SAFE_MODE):
            print('Putting the robot into safe mode...')
            self.toSafeMode()
            time.sleep(0.3)
        if (startingMode == FULL_MODE):
            print('Putting the robot into full mode...')
            self.toSafeMode()
            time.sleep(0.3)
            self.toFullMode()
         
        #changed by alexandra   
        self.serialLock = thread.allocate_lock()

        #self.setLEDs(80,255,0,0) # MB: was 100, want more yellowish        

    def send(self, bytes1):
        if self.in_sim_mode:
            if self.ser:
                #changed by alexandra
                self.ser.write(bytes1)
            #print(bytes1)
            #changed by alexandra
            print (bytes1)
            self.sim_sock.send(bytes1)
        else:
            #self.ser.write( (bytes(bytes1, encoding = 'Latin-1')) )
            self.ser.write(bytes1)
            

    def read(self, bytes):
        message = ""
        if self.in_sim_mode:
            if self.ser:
                self.ser.read( bytes )
            message = self.sim_sock.recv( bytes )
        else:
            message = self.ser.read( bytes )
        return str(message);

    def init_sim_mode(self):
        print('In simulated mode, connecting to simulator socket')
        self.in_sim_mode = True # SRSerial('mapSquare.txt')
        self.sim_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sim_sock.connect((self.sim_host,self.sim_port))

    def reconnect(self,comPort):
        '''
        This method closes the existing connection and reestablishes it.
        When things get bad, this is the only method of recovery.
        '''
        # Just in case it was stuck moving somewhere, stop the Create:
        self.stop()
        # Close the connection:
        self._close()
        # Reestablish the serial connection to the Create:
        self.__init__(comPort)
        self.start()
        time.sleep(0.25) # The recommended 200ms+ pause after mode commands.
        
        if (self.sciMode == SAFE_MODE):
            print('Putting the robot into safe mode...')
            self.toSafeMode()
            time.sleep(0.3)
        if (self.sciMode == FULL_MODE):
            print('Putting the robot into full mode...')
            self.toSafeMode()
            time.sleep(0.3)
            self.toFullMode()
        time.sleep(.25) # The recommended 200ms+ pause after mode commands.


    def start(self):
        """ changes from OFF_MODE to PASSIVE_MODE """
        self.send( START )
        # they recommend 20 ms between mode-changing commands
        time.sleep(0.25)
        # change the mode we think we're in...
        return

    def shutdown(self):
        '''
        This method shuts down the connection to the Create, after first
        stopping the Create and putting the Create into passive mode.
        '''
        self.stop()
        
        self.__sendmsg(COMMANDS["MODE_PASSIVE"],'')

        time.sleep(0.25) # The recommended 200ms+ pause after mode commands.

        self.serialLock.acquire()
        self.start()        # send Create back to passive mode
        time.sleep(0.1)
        if self.in_sim_mode:
            self.sim_sock.close()
        else:
            self.ser.close()
        self.serialLock.release()

    # MB: added back in as private method, since reconnect uses it.
    def _close(self):
        """ tries to shutdown the robot as kindly as possible, by
            clearing any remaining odometric data
            going to passive mode
            closing the serial port
        """
        self.serialLock.acquire()
        self.start()       # send Create back to passive mode
        time.sleep(0.1)
        self.ser.close()
        self.serialLock.release()
        return
    
    def _closeSer(self):
        """ just disconnects the serial port """
        self.serialLock.acquire()
        self.ser.close()
        self.serialLock.release()
        return
    
    def _openSer(self):
        self.serialLock.acquire()
        """ opens the port again """
        self.ser.open()
        self.serialLock.release()
        return

#=============================== Serial Communication 
    def __sendmsg(self, opcode, dataBytes):
        '''
        This method functions as the base of the protocol, sending a message
        with a particular opcode and the given data bytes. opcode should be
        a character; use the constants defined at the top of this file.
        data_bytes must be a string, and should have the proper length
        according to which opcode is used. See the Create serial protocol
        manual for more details.
        '''
#lock
        self.serialLock.acquire()       #note: blocking
        successful = False
        while not successful:
            try:
                self.send(opcode + dataBytes)
                successful = True
            except select.error:
                pass
        self.serialLock.release()
#unlock

    def __sendOpCode(self, opcode):
        '''
        This method functions as the base of the protocol, sending a message
        with a particular opcode and the given data bytes. opcode should be
        a character; use the constants defined at the top of this file.
        data_bytes must be a string, and should have the proper length
        according to which opcode is used. See the Create serial protocol
        manual for more details.
        '''
#lock
        self.serialLock.acquire()       #note: blocking
        successful = False
        while not successful:
            try:
                self.send(opcode)
                successful = True
            except select.error:
                pass
        self.serialLock.release()
#unlock

    def __recvmsg(self, numBytes):
        '''
        This method is used internally for receiving data from the Create.
        It blocks for at most timeout seconds, and then returns as a string
        the bytes of the message received. It reads num_bytes bytes from the
        serial connection. If no message exists, it returns the empty
        string.
        '''
#lock
        self.serialLock.acquire()
        successful = False
        favor = None
        while not successful:
            try:
                favor = self.read(numBytes)
                successful = True
            except select.error:
                pass
        self.serialLock.release()
#unlock
        return favor

    def __sendAndRecvMsg(self,opcode,dataSendBytes,numBytesExpected):
#lock
        self.serialLock.acquire()
        #send
        successful = False
        while not successful:
            try:
                self.send(opcode + dataSendBytes)
                successful = True
            except select.error:
                pass
        #wait?
        #receive
        successful = False
        favor = None
        while not successful:
            try:
                favor = self.read(numBytesExpected)
                successful = True
            except select.error:
                pass
        self.serialLock.release()
#unlock        
        return favor

        
#========================= Moving Around ================================================    
    def stop(self):
        """ stop calls go(0,0) """
        self.go(0,0)

    def go( self, cmPerSec=0, degPerSec=0 ):
        """ go(cmPerSec, degPerSec) sets the robot's linear velocity to
               cmPerSec centimeters per second and its angular velocity to
               degPerSec degrees per second
            go() is equivalent to go(0,0)
        """
        if cmPerSec == 0:
            # just handle rotation
            # convert to radians
            radPerSec = math.radians(degPerSec)
            # make sure the direction is correct
            if radPerSec >= 0:  dirstr = 'CCW'
            else: dirstr = 'CW'
            # compute the velocity, given that the robot's
            # radius is 258mm/2.0
            velMmSec = math.fabs(radPerSec) * (258.0/2.0)
            # send it off to the robot
            self.drive( velMmSec, 0, dirstr )
        
        elif degPerSec == 0:
            # just handle forward/backward translation
            velMmSec = 10.0*cmPerSec
            bigRadius = 32767
            # send it off to the robot
            self.drive( velMmSec, bigRadius )
        
        else:
            # move in the appropriate arc
            radPerSec = math.radians(degPerSec)
            velMmSec = 10.0*cmPerSec
            radiusMm = velMmSec / radPerSec
            # check for extremes
            if radiusMm > 32767: radiusMm = 32767
            if radiusMm < -32767: radiusMm = -32767
            self.drive( velMmSec, radiusMm )
        return
 
    def driveDirect( self, leftCmSec=0, rightCmSec=0 ):
        """ Go(cmpsec, degpsec) sets the robot's velocity to
               cmpsec centimeters per second
               degpsec degrees per second
            Go() is equivalent to go(0,0)
        """
        """ sends velocities of each wheel independently
               left_cm_sec:  left  wheel velocity in cm/sec (capped at +- 50)
               right_cm_sec: right wheel velocity in cm/sec (capped at +- 50)
        """
        if leftCmSec < -50: leftCmSec = -50
        if leftCmSec > 50:  leftCmSec = 50
        if rightCmSec < -50: rightCmSec = -50
        if rightCmSec > 50: rightCmSec = 50
        
        # convert to mm/sec, ensure we have integers
        leftHighVal, leftLowVal = toTwosComplement2Bytes( int(leftCmSec*10) )
        rightHighVal, rightLowVal = toTwosComplement2Bytes( int(rightCmSec*10) )
        # send these bytes and set the stored velocities
        byteList = (rightHighVal,rightLowVal,leftHighVal,leftLowVal)
        if type(byteList) in (list, tuple, set):
            temp = ''
            for char in byteList:
                temp += chr(char)
        byteList = temp
        self.__sendmsg(DRIVEDIRECT,byteList)
        #self.send( DRIVEDIRECT )
        #self.send( chr(rightHighVal) )
        #self.send( chr(rightLowVal) )
        #self.send( chr(leftHighVal) )
        #self.send( chr(leftLowVal) )
        return
        
    def waitTime(self,seconds):
        """ robot waits for the specified time to past (tenths of secs) before executing the next command (CAB)"""
        timeVal= twosComplementInt1byte(int(seconds))
        
        #send the command to the Creeate:
        self.__sendmsg(WAITTIME,chr(timeVal))
    
    def waitEvent(self,eventNumber):
        """ robot waits for the specified event to happen before executing the next command (CAB)"""
        eventVal= twosComplementInt1byte(int(eventNumber))
        
        #Send the command to the Create:
        self.__sendmsg(WAITEVENT,chr(eventVal))
    
    def waitDistance(self,centimeters):
        """ robot waits for the specified distance before executing the next command (CAB)"""
        distInMm = 10*centimeters
        distHighVal, distLowVal=toTwosComplement2Bytes( int(distInMm) )
        
        #Send the command to the Create:
        self.__sendmsg(WAITDIST,chr(distHighVal)+chr(distLowVal))
         
    def waitAngle(self,degrees):
        """ robot waits for the specified angle before executing the next command (CAB)"""
        anglHighVal, anglLowVal=toTwosComplement2Bytes( int(degrees) )
       
        # Send the command for data to the Create:
        self.__sendmsg(WAITANGLE,chr(anglHighVal)+chr(anglLowVal))

    def drive (self, roombaMmSec, roombaRadiusMm, turnDir='CCW'):
        """ implements the drive command as specified
            the turnDir should be either 'CW' or 'CCW' for
            clockwise or counterclockwise - this is only
            used if roombaRadiusMm == 0 (or rounds down to 0)
            other drive-related calls are available
        """
        # first, they should be ints
        #   in case they're being generated mathematically
        if type(roombaMmSec) != type(42):
            roombaMmSec = int(roombaMmSec)
        if type(roombaRadiusMm) != type(42):
            roombaRadiusMm = int(roombaRadiusMm)
        
        # we check that the inputs are within limits
        # if not, we cap them there
        if roombaMmSec < -500:
            roombaMmSec = -500
        if roombaMmSec > 500:
            roombaMmSec = 500
        
        # if the radius is beyond the limits, we go straight
        # it doesn't really seem to go straight, however...
        if roombaRadiusMm < -2000:
            roombaRadiusMm = 32768
        if roombaRadiusMm > 2000:
            roombaRadiusMm = 32768
        
        # get the two bytes from the velocity
        # these come back as numbers, so we will chr them
        velHighVal, velLowVal = toTwosComplement2Bytes( roombaMmSec )
        
        # get the two bytes from the radius in the same way
        # note the special cases
        if roombaRadiusMm == 0:
            if turnDir == 'CW':
                roombaRadiusMm = -1
            else: # default is 'CCW' (turning left)
                roombaRadiusMm = 1
        radiusHighVal, radiusLowVal = toTwosComplement2Bytes( roombaRadiusMm )
        
        #print 'bytes are', velHighVal, velLowVal, radiusHighVal, radiusLowVal
        
        # send these bytes and set the stored velocities
        byteList = (velHighVal,velLowVal,radiusHighVal,radiusLowVal)
        if type(byteList) in (list, tuple, set):
            temp = ''
            for char in byteList:
                temp += chr(char)        
        byteList = temp
        self.__sendmsg(DRIVE,byteList)
        #self.send( DRIVE )
        #self.send( chr(velHighVal) )
        #self.send( chr(velLowVal) )
        #self.send( chr(radiusHighVal) )
        #self.send( chr(radiusLowVal) )

          
#========================== SENSORS ==============================        

    def sensorDataIsOK(self):
        '''Detects data incoherency. Returns false if incoherent ("sensor junk").'''
        # Attempting to reconnect or shutdown the robot from within this
        # function didn't work. Solution is to call the function using syntax:
        # if not robot.sensorDataIsOK():
        #     robot.shutdown()
        #     return (exit before calling other robot code.)
           
        time.sleep(1)
        self.stop()
        self.getSensor('DISTANCE')
        distance = self.getSensor('DISTANCE')
        #Both angle and distance should be ~0. If not, then the sensor was filled 
        #with junk initially, so we reconnect. 
        if abs(distance) > 10:
            #self.reconnect(self.comPort)
            time.sleep(1)
            print("Sensors could not be validated.")
            #self.shutdown()
            return False
        
        return True

    def setMaxSensorTimeout(self, newTimeout):
        ''' Allows the user to wait longer for the robot 
        to return sensor data to the computer. Each retry takes 50 ms.'''
        self.maxSensorRetries = newTimeout / RETRY_SLEEP_TIME
        self.maxSensorRetries = max(newTimeout, MIN_SENSOR_RETRIES)
    
    def getSensor(self, sensorToRead):
        '''Reads the value of the requested sensor from the robot and returns it.'''
        # Send the request for data to the Create:

        self.__sendmsg(COMMANDS["QUERY_LIST"],
                   chr(1) + SENSORS[sensorToRead].ID)
        # Receive the reply:

        # MB: Added ability to retry in case a user is querying the sensors 
        # while the robot is executing a wait command.
        msg = self.__recvmsg(SENSORS[sensorToRead].size)
        nRetries = 0
        while len(msg) < SENSORS[sensorToRead].size and nRetries < self.maxSensorRetries:
            # Serial receive appears to block for 0.5 sec, so we don't
            # need to sleep
            msg = self.__recvmsg(SENSORS[sensorToRead].size)
            nRetries += 1

        #print nRetries, "retries needed"
                    
        # Last resort: return None and force the user to deal with it,
        # rather than crashing.
        if len(msg) < SENSORS[sensorToRead].size:
            #raise CommunicationError("Improper sensor query response length: ")
            #self.close()
            return None
        msg_len = len(msg)
        sensor_bytes = [ord(b) for b in msg[0:msg_len]]

        return self._interpretSensor(sensorToRead,sensor_bytes)

    def _interpretSensor(self, sensorToRead, raw_data):
        '''interprets the raw binary data form a sensor into its appropriate form for use.  This function is for internal use - DO NOT CALL'''
        data = None
        interpret = SENSORS[sensorToRead].interpret

        if len(raw_data) < SENSORS[sensorToRead].size:
                return None

        if interpret == "ONE_BYTE_SIGNED":
            data = self._getOneByteSigned(raw_data[0])
        elif interpret == "ONE_BYTE_UNSIGNED":
            data = self._getOneByteUnsigned(raw_data[0])
        elif interpret == "TWO_BYTE_SIGNED":
            data = self._getTwoBytesSigned(raw_data[0],raw_data[1])
        elif interpret == "TWO_BYTE_UNSIGNED":
            data = self._getTwoBytesUnsigned(raw_data[0],raw_data[1])
        elif interpret == "ONE_BYTE_UNPACK":
            if sensorToRead == "BUMPS_AND_WHEEL_DROPS":
                data = self._getLower5Bits(raw_data[0])
            elif sensorToRead == "BUTTONS":
                data = self._getButtonBits(raw_data[0])
            elif sensorToRead == "USER_DIGITAL_INPUTS":
                data = self._getLower5Bits(raw_data[0])
            if sensorToRead == "OVERCURRENTS":
                data = self._getLower5Bits(raw_data[0])
        elif interpret == "NO_HANDLING":
            data = raw_data

        return data
#======================= CARGO BAY OUTPUTS ==========================

    def setDigitalOutputs(self, digOut2, digOut1, digOut0):
        '''sets the digital output pins of the cargo bay connector to the specifed value (1 or 0)'''
        data_byte = int("00000"+str(digOut2)+str(digOut1)+str(digOut0),2)
        self.__sendmsg(COMMANDS["DIGITAL_OUTPUTS"],chr(data_byte))

    def setLowSideDrivers(self, driver2, driver1, driver0):
        '''sets the low side driver output pins of the cargo bay connector to the specifed value (1 or 0)'''
        data_byte = int("00000"+str(driver2)+str(driver1)+str(driver0),2)
        self.__sendmsg(COMMANDS["LOW_SIDE_DRIVERS"],chr(data_byte))

    def setPWMLowSideDrivers(self, dutyCycle2, dutyCycle1, dutyCycle0):
        '''sets the low side driver output pins of the cargo bay connector to the specifed value (0 to 255)'''
        self.__sendmsg(COMMANDS["PWM_LOW_SIDE_DRIVERS"],
                       chr(dutyCycle2)+chr(dutyCycle1)+chr(dutyCycle0))

    def sendIR(self,byteValue):
        ''' send the requested byte out of low side driver 1 (pin 23 on Cargo Bay Connector) (0-255) '''
        self.__sendmsg(COMMANDS["SEND_IR"], chr(byteValue))

    def startIR(self,byteValue):
        '''TODO: implement script send to begin sending passed value'''
        """Uses a script so that the robot can receive and perform other 
        commands concurrently. Alternative to threading. """
        print("sending byte", byteValue)
   
        byteList = chr(3); #  # script has 3 bytes
        byteList += COMMANDS["SEND_IR"]
        byteList += chr(byteValue) # IR value
        byteList += RUN_SCRIPT #(running at end of def sets up recursion)
        self.__sendmsg(DEFINE_SCRIPT,byteList)
        self.__sendOpCode(RUN_SCRIPT) #actually run the script

    def stopIR(self):
        '''TO DO: send null script to end IR streaming'''
        """Uses a script so that the robot can receive and perform other 
        commands concurrently. Alternative to threading. """
        self.__sendmsg(DEFINE_SCRIPT, chr(0)) #define null script
        
#========================== LIGHTS ==================================    
    def setLEDs(self, powerColor, powerIntensity, play, advance ):
        """ The setLEDs method sets each of the three LEDs, from left to right:
            the power LED, the play LED, and the status LED.
            The power LED at the left can display colors from green (0) to red (255)
            and its intensity can be specified, as well. Hence, power_color and
            power_intensity are values from 0 to 255. The other two LED inputs
            should either be 0 (off) or 1 (on).
        """
        
        # make sure we're within range...
        if advance != 0: advance = 1
        if play != 0: play = 1
        try:
            power = int(powerIntensity)
            powercolor = int(powerColor)
        except TypeError:
            power = 128
            powercolor = 128
            print('Type exception caught in setAbsoluteLEDs in roomba.py')
            print('Your powerColor or powerIntensity was not an integer.')
        if power < 0: power = 0
        if power > 255: power = 255
        if powercolor < 0: powercolor = 0
        if powercolor > 255: powercolor = 255
        # create the first byte
        #firstByteVal = (status << 4) | (spot << 3) | (clean << 2) | (max << 1) | dirtdetect
        firstByteVal =  (advance << 3) | (play << 1) 
        
        # send these as bytes
        # print 'bytes are', firstByteVal, powercolor, power
        self.send( LEDS )
        self.send( chr(firstByteVal) )
        self.send( chr(powercolor) )
        self.send( chr(power) )
        
        return

#==================== DEMOS ======================     
    def seekDock(self):
        """sends the force-seeking-dock signal """
        self.demo(1)

    
    def demo(self, demoNumber=-1):
        """ runs one of the built-in demos for Create
            if demoNumber is
              <omitted> or
              -1 stop current demo
               0 wander the surrounding area
               1 wander and dock, when the docking station is seen
               2 wander a more local area
               3 wander to a wall and then follow along it
               4 figure 8
               5 "wimp" demo: when pushed, move forward
                 when bumped, move back and away
               6 home: will home in on a virtual wall, as
                 long as the back and sides of the IR receiver
                 are covered with tape
               7 tag: homes in on sequential virtual walls
               8 pachelbel: plays the first few notes of the canon in D
               9 banjo: plays chord notes according to its cliff sensors
                 chord key is selected via the bumper
        """
        if (demoNumber < -1 or demoNumber > 9):
            demoNumber = -1 # stop current demo
        
        self.send( DEMO )
        if demoNumber < 0 or demoNumber > 9:
            # invalid values are equivalent to stopping
            self.send( chr(255) ) # -1
        else:
            self.send( chr(demoNumber) )

#==================== MUSIC ======================     
    def setSong(self, songNumber, noteList):
        """ this stores a song to roomba's memory to play later
           with the playSong command
           songNumber must be between 0 and 15 (inclusive)
           songDataList is a list of (note, duration) pairs (up to 16)
           note is the midi note number, from 31 to 127
           (outside this range, the note is a rest)
           duration is from 0 to 255 in 1/64ths of a second
        """
        # any notes to play?
        if type(noteList) != type([]) and type(noteList) != type(()):
            print('noteList was', noteList)
            return 
            
        if len(noteList) < 1:
            print('No data in the noteList')
            return
        
        if songNumber < 0: songNumber = 0
        if songNumber > 15: songNumber = 15
        
        # indicate that a song is coming
        self.send( SONG )
        self.send( chr(songNumber) )
        
        L = min(len(noteList), 16)
        self.send( chr(L) )
        
        # loop through the notes, up to 16
        for note in noteList[:L]:
            # make sure its a tuple, or else we rest for 1/4 second
            if type(note) == type( () ):
                #more error checking here!
                self.send( chr(note[0]) )  # note number
                self.send( chr(note[1]) )  # duration
            else:
                self.send( chr(30) )   # a rest note
                self.send( chr(16) )   # 1/4 of a second
                
        return
        
    def playSong(self, noteList):
        """ The input to <tt>playSong</tt> should be specified as a list
            of pairs of ( note_number, note_duration ) format. Thus, 
            r.playSong( [(60,8),(64,8),(67,8),(72,8)] ) plays a quick C chord.
        """
        # implemented by setting song #1 to the notes and then playing it
        self.setSong(1, noteList)
        self.playSongNumber(1)
    
    def playSongNumber(self, songNumber):
        """ plays song songNumber """
        if songNumber < 0: songNumber = 0
        if songNumber > 15: songNumber = 15
        
        self.send( PLAY )
        self.send( chr(songNumber) )
    
    def playNote(self, noteNumber, duration, songNumber=0):
        """ plays a single note as a song (at songNumber)
            duration is in 64ths of a second (1-255)
            the note number chart is on page 12 of the open interface manual
        """
        # set the song
        self.setSong(songNumber, [(noteNumber,duration)])
        self.playSongNumber(songNumber)

#==================== Modes ======================    
    def toSafeMode(self):
        """ changes the state (from PASSIVE_MODE or FULL_MODE)
            to SAFE_MODE
        """
        self.start()
        time.sleep(0.03)
        # now we're in PASSIVE_MODE, so we repeat the above code...
        self.send( SAFE )
        # they recommend 20 ms between mode-changing commands
        time.sleep(0.03)
        # change the mode we think we're in...
        self.sciMode = SAFE_MODE
        # no response here, so we don't get any...
        return
    
    def toFullMode(self):
        """ changes the state from PASSIVE to SAFE to FULL_MODE
        """
        self.start()
        time.sleep(0.03)
        self.toSafeMode()
        time.sleep(0.03)
        self.send( FULL )
        time.sleep(0.03)
        self.sciMode = FULL_MODE
        
        return     
    
    #==================== Class Level Math functions =============
    def _getButtonBits( self, r ):
        """ r is one byte as an integer """
        return [ bitOfByte(2,r), bitOfByte(0,r) ]
    
    def _getLower5Bits( self, r ):
        """ r is one byte as an integer """
        return [ bitOfByte(4,r), bitOfByte(3,r), bitOfByte(2,r), bitOfByte(1,r), bitOfByte(0,r) ]

    def _getOneBit( self, r ):
        """ r is one byte as an integer """
        if r == 1:  return 1
        else:       return 0
     
    def _getOneByteSigned( self, r ):
        """ r is one byte as a signed integer """
        return twosComplementInt1byte( r )

    def _getOneByteUnsigned( self, r ):
        """ r is one byte as an integer """
        return r
    
    def _getTwoBytesSigned( self, r1, r2 ):
        """ r1, r2 are two bytes as a signed integer """
        return twosComplementInt2bytes( r1, r2 )

    def _getTwoBytesUnsigned( self, r1, r2 ):
        """ r1, r2 are two bytes as an unsigned integer """
        return r1 << 8 | r2
        
    def _rawSend( self, listofints ):
        for x in listofints:
            self.send( chr(x) )
    
    def _rawRecv( self ):
        nBytesWaiting = self.ser.inWaiting()
        #print 'nBytesWaiting is', nBytesWaiting
        r = self.read(nBytesWaiting)
        r = [ ord(x) for x in r ]
        #print 'r is', r
        return r
    
    def _rawRecvStr( self ):
        nBytesWaiting = self.ser.inWaiting()
        #print 'nBytesWaiting is', nBytesWaiting
        r = self.ser.read(nBytesWaiting)
        return r

    def getMode(self):
        """ returns one of OFF_MODE, PASSIVE_MODE, SAFE_MODE, FULL_MODE """
        # but how right is it?
        return self.sciMode
    
if __name__ == '__main__':
    displayVersion()
