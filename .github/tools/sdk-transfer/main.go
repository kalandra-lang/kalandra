// CI-only authenticated SDK transport. Private receiver keys never leave their job.
package main

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/binary"
	"encoding/pem"
	"errors"
	"fmt"
	"os"
)

var magic = []byte("KALSDK1\n")

const maxPayload = 512 << 20

func read(path string) ([]byte, error) {
	info, err := os.Stat(path)
	if err != nil {
		return nil, err
	}
	if info.Size() > maxPayload {
		return nil, errors.New("SDK transfer exceeds bound")
	}
	return os.ReadFile(path)
}
func write(path string, b []byte) error { return os.WriteFile(path, b, 0600) }
func generate(private, public string) error {
	key, err := rsa.GenerateKey(rand.Reader, 3072)
	if err != nil {
		return err
	}
	encoded, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		return err
	}
	if err = write(private, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: encoded})); err != nil {
		return err
	}
	encoded, err = x509.MarshalPKIXPublicKey(&key.PublicKey)
	if err != nil {
		return err
	}
	return write(public, pem.EncodeToMemory(&pem.Block{Type: "PUBLIC KEY", Bytes: encoded}))
}
func seal(public, input, output string) error {
	data, err := read(public)
	if err != nil {
		return err
	}
	block, _ := pem.Decode(data)
	if block == nil {
		return errors.New("invalid receiver key")
	}
	parsed, err := x509.ParsePKIXPublicKey(block.Bytes)
	if err != nil {
		return err
	}
	key, ok := parsed.(*rsa.PublicKey)
	if !ok || key.N.BitLen() < 3072 {
		return errors.New("unsupported receiver key")
	}
	secret := make([]byte, 32)
	if _, err = rand.Read(secret); err != nil {
		return err
	}
	wrapped, err := rsa.EncryptOAEP(sha256.New(), rand.Reader, key, secret, magic)
	if err != nil {
		return err
	}
	algorithm, err := aes.NewCipher(secret)
	if err != nil {
		return err
	}
	aead, err := cipher.NewGCM(algorithm)
	if err != nil {
		return err
	}
	nonce := make([]byte, aead.NonceSize())
	if _, err = rand.Read(nonce); err != nil {
		return err
	}
	plain, err := read(input)
	if err != nil {
		return err
	}
	if len(plain) > maxPayload-8192 {
		return errors.New("SDK plaintext exceeds envelope bound")
	}
	header := append([]byte{}, magic...)
	header = binary.BigEndian.AppendUint32(header, uint32(len(wrapped)))
	header = append(header, wrapped...)
	header = append(header, nonce...)
	result := append([]byte{}, header...)
	result = aead.Seal(result, nonce, plain, header)
	return write(output, result)
}
func open(private, input, output string) error {
	data, err := read(private)
	if err != nil {
		return err
	}
	block, _ := pem.Decode(data)
	if block == nil {
		return errors.New("invalid private key")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return err
	}
	key, ok := parsed.(*rsa.PrivateKey)
	if !ok {
		return errors.New("unsupported private key")
	}
	data, err = read(input)
	if err != nil {
		return err
	}
	if len(data) < len(magic)+4 || !bytes.Equal(data[:len(magic)], magic) {
		return errors.New("invalid transfer header")
	}
	n := int(binary.BigEndian.Uint32(data[len(magic) : len(magic)+4]))
	offset := len(magic) + 4
	if n < 256 || n > 4096 || len(data) < offset+n+12+16 {
		return errors.New("invalid transfer length")
	}
	secret, err := rsa.DecryptOAEP(sha256.New(), rand.Reader, key, data[offset:offset+n], magic)
	if err != nil {
		return err
	}
	algorithm, err := aes.NewCipher(secret)
	if err != nil {
		return err
	}
	aead, err := cipher.NewGCM(algorithm)
	if err != nil {
		return err
	}
	end := offset + n + aead.NonceSize()
	nonce := data[offset+n : end]
	plain, err := aead.Open(nil, nonce, data[end:], data[:end])
	if err != nil {
		return err
	}
	return write(output, plain)
}
func main() {
	args := os.Args
	var err error
	switch {
	case len(args) == 4 && args[1] == "key":
		err = generate(args[2], args[3])
	case len(args) == 5 && args[1] == "seal":
		err = seal(args[2], args[3], args[4])
	case len(args) == 5 && args[1] == "open":
		err = open(args[2], args[3], args[4])
	default:
		err = errors.New("usage: sdk-transfer key PRIVATE PUBLIC | seal PUBLIC INPUT OUTPUT | open PRIVATE INPUT OUTPUT")
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
